// MIT License © 2025 Binary Dice Games
/**
 * @file stdio_transport.cpp
 * @brief libuv-backed implementation of the `BISON:` line-framed stdio
 *        transport. See stdio_transport.hpp and FORMAT.md for the framing
 *        contract.
 */
#include "src/rmi/transport/stdio_transport.hpp"
#include "src/rmi/transport/uv_stream_state.hpp"

#include <uv.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace bdg::bison::rmi::transport {

namespace {

// ── Base64 (internal helper, not a public bison:: API) ────────────────────────

constexpr char kBase64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const bison::buffer& in) {
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= in.size()) {
    const uint32_t n = (static_cast<uint32_t>(in[i]) << 16) | (static_cast<uint32_t>(in[i + 1]) << 8) |
                        static_cast<uint32_t>(in[i + 2]);
    out.push_back(kBase64Chars[(n >> 18) & 0x3F]);
    out.push_back(kBase64Chars[(n >> 12) & 0x3F]);
    out.push_back(kBase64Chars[(n >> 6) & 0x3F]);
    out.push_back(kBase64Chars[n & 0x3F]);
    i += 3;
  }
  const size_t rem = in.size() - i;
  if (rem == 1) {
    const uint32_t n = static_cast<uint32_t>(in[i]) << 16;
    out.push_back(kBase64Chars[(n >> 18) & 0x3F]);
    out.push_back(kBase64Chars[(n >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    const uint32_t n = (static_cast<uint32_t>(in[i]) << 16) | (static_cast<uint32_t>(in[i + 1]) << 8);
    out.push_back(kBase64Chars[(n >> 18) & 0x3F]);
    out.push_back(kBase64Chars[(n >> 12) & 0x3F]);
    out.push_back(kBase64Chars[(n >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

constexpr int8_t kDecodeTableSize = 80; // '+' (0x2B) through 'z' (0x7A)

std::array<int8_t, kDecodeTableSize> make_decode_table() {
  std::array<int8_t, kDecodeTableSize> table{};
  table.fill(-1);
  for (int8_t i = 0; i < 64; ++i)
    table[static_cast<unsigned char>(kBase64Chars[i]) - '+'] = i;
  return table;
}

/**
 * @brief Decode a base64 string.
 * @return The decoded bytes, or std::nullopt if the input is malformed
 *         (invalid character or invalid length/padding).
 */
std::optional<bison::buffer> base64_decode(std::string_view in) {
  static const auto table = make_decode_table();
  if (in.empty())
    return bison::buffer{};
  if (in.size() % 4 != 0)
    return std::nullopt;

  bison::buffer out;
  out.reserve((in.size() / 4) * 3);

  for (size_t i = 0; i < in.size(); i += 4) {
    int32_t vals[4];
    int pad = 0;
    for (int j = 0; j < 4; ++j) {
      const unsigned char c = static_cast<unsigned char>(in[i + j]);
      if (c == '=') {
        vals[j] = 0;
        ++pad;
      } else {
        if (c < '+' || c >= '+' + kDecodeTableSize || table[c - '+'] < 0)
          return std::nullopt;
        if (pad > 0)
          return std::nullopt; // '=' padding must be a suffix
        vals[j] = table[c - '+'];
      }
    }
    if (pad > 2 || (pad > 0 && i + 4 != in.size()))
      return std::nullopt; // padding only allowed in the final block

    const uint32_t n = (static_cast<uint32_t>(vals[0]) << 18) | (static_cast<uint32_t>(vals[1]) << 12) |
                        (static_cast<uint32_t>(vals[2]) << 6) | static_cast<uint32_t>(vals[3]);
    out.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
    if (pad < 2)
      out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    if (pad < 1)
      out.push_back(static_cast<uint8_t>(n & 0xFF));
  }
  return out;
}

constexpr std::string_view kFramePrefix = "\nBISON:";

// ── Connect-time handshake (see stdio_client_transport::open()'s doc comment) ──

// Terminated with "\r\n", not bare "\n": these are sent with raw_mode_guard
// already active (OPOST off — see its doc comment), so nothing else adds
// the "\r" a terminal needs to return to the left margin. Unlike the
// BISON:-frame-adjacent bytes elsewhere in this file, there's no risk of
// this "\r" itself being misread as data — these are fixed, complete,
// human-readable lines, not part of the base64 frame payload.
constexpr std::string_view kHandshakeStart = "START BISON/1.0\r\n";
constexpr std::string_view kHandshakeOk = "BISON/1.0 OK\r\n";
constexpr std::string_view kHandshakeStop = "STOP BISON/1.0\r\n";

// Bound on how many passthrough bytes the handshake watchers above will
// accumulate while looking for their target line. Both watchers only ever
// need to hold back up to (target.size() - 1) bytes to keep a split match
// intact; this is a generous multiple of the longest target so a burst of
// unrelated passthrough noise arriving alongside the target can't ever
// truncate a match, while still bounding memory if the target never shows.
constexpr size_t kHandshakeAccumCap = 256;

} // namespace

/**
 * @brief Duplicate @p fd so a libuv pipe handle can own the duplicate.
 *
 * `uv_close()` on a `uv_pipe_t` opened with `uv_pipe_open()` closes the
 * wrapped fd. That's fine when the fd is exclusively the transport's own
 * (e.g. plain stdin/stdout), but `read_fd_`/`write_fd_` can be a fd another
 * component still owns and needs to keep open past this connection's
 * lifetime — a pty master fd shared with `pty_process` (see
 * `src/pty/DESIGN.md`) being the motivating case: without duplicating it
 * here, a client disconnecting (`stdio_server_connection::close()`) would
 * close the master fd out from under `pty_process`, which both invalidates
 * its `write()`s from `pump_loop()` and hangs up the pty's session (the
 * spawned shell), for what should be just one RMI session ending. Wrapping
 * a duplicate instead means closing the handle only ever closes the
 * duplicate, never the caller's original fd. Implemented per-platform
 * (`stdio_transport_linux.cpp`/`stdio_transport_win.cpp`) since Linux and
 * Windows CRTs spell "duplicate an fd" differently.
 */
int dup_stdio_fd(int fd);

// ── stdio_pipe_thread: shared loop/thread lifecycle ─────────────────────────────

/**
 * @brief Owns one libuv loop, one uv_pipe_t wrapping a duplicate of a
 *        caller-supplied fd, and the background thread that runs the loop.
 *
 * Factors out the loop-init/pipe-open/thread-start/stop-and-join sequence
 * that stdio_reader and stdio_writer each need, but with their own
 * direction-specific pre/post steps around uv_run().
 */
struct stdio_pipe_thread {
  uv_loop_t loop{};
  uv_pipe_t handle{};
  uv_async_t stop_async{};
  std::atomic<bool> stopped{false};
  std::thread loop_thread;

  void init_pipe(int fd, const char* which, uv_async_cb on_stop, void* owner) {
    uv_loop_init(&loop);
    uv_pipe_init(&loop, &handle, 0);
    const int dup_fd = dup_stdio_fd(fd);
    if (dup_fd < 0)
      throw std::runtime_error(std::string{"stdio_transport: dup ("} + which + ") failed");
    const int r = uv_pipe_open(&handle, dup_fd);
    if (r != 0)
      throw std::runtime_error(std::string{"stdio_transport: uv_pipe_open ("} + which + ") failed: " + uv_strerror(r));
    handle.data = owner;
    uv_async_init(&loop, &stop_async, on_stop);
    stop_async.data = owner;
  }

  template <typename PreRun, typename PostRun>
  void start(PreRun&& pre_run, PostRun&& post_run) {
    loop_thread = std::thread([this, pre_run = std::forward<PreRun>(pre_run),
                                post_run = std::forward<PostRun>(post_run)] {
      pre_run();
      uv_run(&loop, UV_RUN_DEFAULT);
      post_run();
      uv_loop_close(&loop);
    });
  }

  void stop() {
    if (stopped.exchange(true))
      return;
    if (loop_thread.joinable()) {
      uv_async_send(&stop_async);
      loop_thread.join();
    }
  }
};

// ── Reader: BISON:-line scanner over one fd ────────────────────────────────────

/**
 * @brief Dedicated read-only libuv loop/thread for one fd.
 *
 * Runs a byte-level scanner (see stdio_transport.hpp's doc comment and
 * FORMAT.md) that forwards non-frame bytes to `passthrough` immediately and
 * enqueues decoded frames onto `recv_queue`.
 */
struct stdio_reader {
  // How long a byte sequence that's a tentative-but-incomplete match against
  // kFramePrefix (e.g. a lone trailing '\n') is held before being flushed to
  // `passthrough` anyway. Real BISON: frames are written as a single line in
  // one shot, so their bytes arrive together well within this window; a
  // human's keystrokes (or any other source that pauses mid-stream) don't
  // get stuck waiting for a byte that disambiguates the match.
  static constexpr uint64_t kIdleFlushMs = 50;

  stdio_pipe_thread io;
  std::vector<uint8_t> read_buf = std::vector<uint8_t>(65536);
  uv_timer_t idle_timer{};

  // Scanner state — loop thread only.
  bool in_frame{false};
  size_t match_pos{1}; // pretend kFramePrefix[0] already matched at stream start
  std::string speculative;
  std::string payload;

  stdio_passthrough_cb passthrough;
  bison::synchronized<std::queue<bison::buffer>> recv_queue;
  std::atomic<bool> recv_closed{false};

  stdio_reader() = default;
  ~stdio_reader() { stop(); }
  stdio_reader(const stdio_reader&) = delete;
  stdio_reader& operator=(const stdio_reader&) = delete;

  void init(int fd, stdio_passthrough_cb cb) {
    passthrough = std::move(cb);
    io.init_pipe(fd, "read", on_stop, this);
    uv_timer_init(&io.loop, &idle_timer);
    idle_timer.data = this;
  }

  void start_loop() {
    io.start(
        [this] { uv_read_start(reinterpret_cast<uv_stream_t*>(&io.handle), alloc_cb, on_read); },
        [this] {
          recv_closed.store(true);
          recv_queue.notify_all();
        });
  }

  void stop() { io.stop(); }

  bool dequeue_frame(bison::buffer& frame, std::chrono::milliseconds timeout) {
    bool got = false;
    recv_queue.wait_for(timeout, [&](auto& q) {
      if (q.empty() && !recv_closed.load())
        return false;
      if (!q.empty()) {
        frame = std::move(q.front());
        q.pop();
        got = true;
      }
      return true;
    });
    return got;
  }

  // ── Scanner ──────────────────────────────────────────────────────────────

  void feed_byte(uint8_t b) {
    if (in_frame) {
      if (b == '\n') {
        complete_frame();
      } else {
        payload.push_back(static_cast<char>(b));
      }
      return;
    }

    if (match_pos < kFramePrefix.size() && b == static_cast<uint8_t>(kFramePrefix[match_pos])) {
      speculative.push_back(static_cast<char>(b));
      ++match_pos;
      if (match_pos == kFramePrefix.size()) {
        in_frame = true;
        speculative.clear();
        payload.clear();
        uv_timer_stop(&idle_timer);
      } else {
        arm_idle_flush();
      }
      return;
    }

    // Mismatch: flush whatever was speculatively held, then reprocess this
    // byte fresh (it may itself be the '\n' that starts a new attempt).
    if (!speculative.empty()) {
      passthrough(speculative);
      speculative.clear();
    }
    match_pos = 0;

    if (b == '\n') {
      match_pos = 1;
      speculative.push_back(static_cast<char>(b));
      arm_idle_flush();
    } else {
      uv_timer_stop(&idle_timer);
      const char c = static_cast<char>(b);
      passthrough(std::string_view{&c, 1});
    }
  }

  /**
   * @brief (Re)start the idle-flush timer: if no further byte arrives within
   *        `kIdleFlushMs`, the held `speculative` bytes are flushed to
   *        `passthrough` as though a mismatch had occurred.
   */
  void arm_idle_flush() { uv_timer_start(&idle_timer, on_idle_timeout, kIdleFlushMs, 0); }

  static void on_idle_timeout(uv_timer_t* h) {
    auto* self = static_cast<stdio_reader*>(h->data);
    if (!self->speculative.empty()) {
      self->passthrough(self->speculative);
      self->speculative.clear();
    }
    self->match_pos = 0;
  }

  void complete_frame() {
    auto decoded = base64_decode(payload);
    if (!decoded) {
      std::cerr << "[stdio_transport] warning: malformed BISON: frame ignored\n";
      std::string line = "BISON:" + payload + "\n";
      passthrough(line);
    } else {
      recv_queue.withWLock([&](auto& q) { q.push(std::move(*decoded)); });
      recv_queue.notify_one();
    }
    payload.clear();
    in_frame = false;
    match_pos = 1; // a '\n' just closed the frame; treat it as already matched
    speculative.clear();
  }

  // ── Static libuv callbacks ───────────────────────────────────────────────

  static void alloc_cb(uv_handle_t* h, size_t /*suggested*/, uv_buf_t* buf) {
    auto* self = static_cast<stdio_reader*>(h->data);
    buf->base = reinterpret_cast<char*>(self->read_buf.data());
    buf->len = static_cast<decltype(buf->len)>(self->read_buf.size());
  }

  static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* /*buf*/) {
    auto* self = static_cast<stdio_reader*>(stream->data);
    if (nread < 0) {
      self->recv_closed.store(true);
      self->recv_queue.notify_all();
      self->passthrough(std::string_view{}); // signals closure; see stdio_passthrough_cb's doc comment
      uv_read_stop(stream);
      return;
    }
    if (nread == 0)
      return;
    for (ssize_t i = 0; i < nread; ++i)
      self->feed_byte(self->read_buf[static_cast<size_t>(i)]);
  }

  static void on_stop(uv_async_t* h) {
    auto* self = static_cast<stdio_reader*>(h->data);
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(&self->io.handle));
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(&self->io.stop_async));
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(&self->idle_timer));
  }
};

// ── Writer: raw byte sink over one fd ───────────────────────────────────────────

/** @brief Dedicated write-only libuv loop/thread for one fd. */
struct stdio_writer {
  stdio_pipe_thread io;
  uv_async_t send_async{};

  bison::synchronized<std::queue<std::vector<uint8_t>>> send_queue;
  std::atomic<bool> stopped{false};

  stdio_writer() = default;
  ~stdio_writer() { stop(); }
  stdio_writer(const stdio_writer&) = delete;
  stdio_writer& operator=(const stdio_writer&) = delete;

  void init(int fd) {
    io.init_pipe(fd, "write", on_stop, this);
    uv_async_init(&io.loop, &send_async, on_send);
    send_async.data = this;
  }

  void start_loop() {
    io.start([] {}, [] {});
  }

  void stop() {
    if (stopped.exchange(true))
      return;
    io.stop();
  }

  void enqueue_bytes(std::vector<uint8_t> data) {
    if (stopped.load())
      return;
    send_queue.withWLock([&](auto& q) { q.push(std::move(data)); });
    uv_async_send(&send_async);
  }

  static void on_send(uv_async_t* h) {
    auto* self = static_cast<stdio_writer*>(h->data);
    uv_flush_write_queue(self->send_queue, reinterpret_cast<uv_stream_t*>(&self->io.handle));
  }

  static void on_stop(uv_async_t* h) {
    auto* self = static_cast<stdio_writer*>(h->data);
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(&self->io.handle));
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(&self->send_async));
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(&self->io.stop_async));
  }
};

// ── stdio_conn_state ────────────────────────────────────────────────────────────

struct stdio_conn_state {
  stdio_reader reader;
  stdio_writer writer;
  std::atomic<bool> closed{false};

  // ── Client-side connect handshake ──────────────────────────────────────
  // Gates the caller's passthrough callback until "BISON/1.0 OK\n" is seen
  // (or open() gives up), so a bogus "peer" line never reaches, say,
  // client_app's REPL input queue. See stdio_client_transport::open().
  struct client_handshake_state {
    bool done = false;
    std::string accum;
  };
  bison::synchronized<client_handshake_state> client_handshake;

  // ── Server-side handshake watcher ──────────────────────────────────────
  // Never gates anything — just watches a *copy* of every passthrough chunk
  // for "START BISON/1.0\n" and replies. See stdio_server_transport's doc
  // comment for why this doesn't need to suppress the line the way the
  // client side does.
  bison::synchronized<std::string> server_handshake_accum;

  void start(int read_fd, int write_fd, stdio_passthrough_cb passthrough) {
    reader.init(read_fd, std::move(passthrough));
    writer.init(write_fd);
    reader.start_loop();
    writer.start_loop();
  }

  void send_frame(const bison::buffer& frame) {
    if (closed.load())
      throw std::runtime_error("stdio_transport: send on closed connection");
    std::string line;
    line.reserve(kFramePrefix.size() + ((frame.size() + 2) / 3) * 4 + 1);
    line.append(kFramePrefix);
    line.append(base64_encode(frame));
    line.push_back('\n');
    writer.enqueue_bytes(std::vector<uint8_t>(line.begin(), line.end()));
  }

  void send_raw(std::string_view bytes) {
    if (closed.load())
      return;
    writer.enqueue_bytes(std::vector<uint8_t>(bytes.begin(), bytes.end()));
  }

  /**
   * @brief Client-side passthrough wrapper: withholds everything from
   *        @p real until `"BISON/1.0 OK\n"` has been seen in the stream,
   *        at which point it delivers any bytes that arrived immediately
   *        after that line (in the same chunk) and becomes a transparent
   *        pass-through for the rest of the connection's lifetime.
   */
  void client_passthrough(std::string_view chunk, const stdio_passthrough_cb& real) {
    if (chunk.empty()) { // stream-closed signal — must still propagate even mid-handshake
      real(chunk);
      return;
    }
    bool already_done = false;
    bool just_resolved = false;
    std::string leftover;
    client_handshake.withWLock([&](auto& hs) {
      if (hs.done) {
        already_done = true;
        return;
      }
      hs.accum.append(chunk);
      const auto pos = hs.accum.find(kHandshakeOk);
      if (pos != std::string::npos) {
        hs.done = true;
        just_resolved = true;
        leftover = hs.accum.substr(pos + kHandshakeOk.size());
        hs.accum.clear();
      } else if (hs.accum.size() > kHandshakeAccumCap) {
        hs.accum.erase(0, hs.accum.size() - kHandshakeAccumCap);
      }
    });
    if (already_done) {
      real(chunk);
      return;
    }
    if (just_resolved) {
      client_handshake.notify_all();
      if (!leftover.empty())
        real(leftover);
    }
    // Still waiting: withhold silently.
  }

  /** @brief Blocks until client_passthrough() has seen "BISON/1.0 OK\n", or @p timeout elapses. */
  bool wait_for_client_handshake(std::chrono::milliseconds timeout) {
    return client_handshake.wait_for(timeout, [](const auto& hs) { return hs.done; });
  }

  /**
   * @brief Server-side passthrough tap: watches (without withholding
   *        anything) for `"START BISON/1.0\n"`, replying with
   *        `"BISON/1.0 OK\n"` each time it appears.
   */
  void watch_for_handshake_start(std::string_view chunk) {
    if (chunk.empty())
      return;
    bool matched = false;
    server_handshake_accum.withWLock([&](std::string& accum) {
      accum.append(chunk);
      const auto pos = accum.find(kHandshakeStart);
      if (pos != std::string::npos) {
        matched = true;
        accum.erase(0, pos + kHandshakeStart.size());
      }
      if (accum.size() > kHandshakeAccumCap)
        accum.erase(0, accum.size() - kHandshakeAccumCap);
    });
    if (matched)
      send_raw(kHandshakeOk);
  }

  bool receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
    if (closed.load())
      return false;
    return reader.dequeue_frame(frame, timeout);
  }

  void stop() {
    closed.store(true);
    reader.stop();
    writer.stop();
  }
};

// ── Default passthrough callbacks ───────────────────────────────────────────────

void stdio_print_passthrough(std::string_view chunk) {
  std::cout.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
  std::cout.flush();
}

void stdio_discard_passthrough(std::string_view /*chunk*/) {}

// ── stdio_client_transport ─────────────────────────────────────────────────────

stdio_client_transport::stdio_client_transport(int read_fd, int write_fd, stdio_passthrough_cb passthrough,
                                                std::chrono::milliseconds handshake_timeout)
    : state_(std::make_unique<stdio_conn_state>()), handshake_timeout_(handshake_timeout) {
  // Wrap immediately (not deferred to open()) so the handshake gate is
  // active from the very first byte the reader ever sees — open() typically
  // runs microseconds later (client::connect() calls it right after
  // construction), but nothing guarantees that ordering at the type level.
  auto* state_ptr = state_.get();
  state_->start(read_fd, write_fd, [state_ptr, real_passthrough = std::move(passthrough)](std::string_view chunk) {
    state_ptr->client_passthrough(chunk, real_passthrough);
  });
}

stdio_client_transport::~stdio_client_transport() {
  if (state_)
    state_->stop();
}

void stdio_client_transport::open(bison::dynamic /*params*/) {
  state_->send_raw(kHandshakeStart);
  if (!state_->wait_for_client_handshake(handshake_timeout_)) {
    throw std::runtime_error(
        "stdio_transport: handshake timed out — sent START BISON/1.0 but got no BISON/1.0 OK "
        "from the peer (is there a bison_server --pty running on the other end?)");
  }
}

void stdio_client_transport::send(bison::buffer frame) {
  state_->send_frame(frame);
}

void stdio_client_transport::send_raw(std::string_view bytes) {
  state_->send_raw(bytes);
}

bool stdio_client_transport::receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
  return state_->receive(frame, timeout);
}

void stdio_client_transport::shutdown() {
  state_->send_raw(kHandshakeStop);
  state_->stop();
}

// ── stdio_server_connection ───────────────────────────────────────────────────

stdio_server_connection::stdio_server_connection(std::shared_ptr<stdio_conn_state> state, std::function<void()> on_close)
    : state_(std::move(state)), on_close_(std::move(on_close)) {}

stdio_server_connection::~stdio_server_connection() {
  close();
}

void stdio_server_connection::send(bison::buffer frame) {
  state_->send_frame(frame);
}

bool stdio_server_connection::receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
  if (closed_.load())
    return false;
  return state_->receive(frame, timeout);
}

void stdio_server_connection::close() {
  if (closed_.exchange(true))
    return;
  if (on_close_)
    on_close_();
}

bool stdio_server_connection::is_closed() const {
  return closed_.load();
}

// ── stdio_server_transport ────────────────────────────────────────────────────

stdio_server_transport::stdio_server_transport(int read_fd, int write_fd, stdio_passthrough_cb passthrough)
    : read_fd_(read_fd), write_fd_(write_fd), passthrough_(std::move(passthrough)) {}

stdio_server_transport::~stdio_server_transport() {
  if (state_)
    state_->stop();
}

void stdio_server_transport::start(bison::dynamic /*params*/) {
  stopped_.store(false);
  state_ = std::make_shared<stdio_conn_state>();
  auto* state_ptr = state_.get();
  state_->start(read_fd_, write_fd_, [state_ptr, real_passthrough = passthrough_](std::string_view chunk) {
    real_passthrough(chunk); // unchanged, always forward — see the class doc comment
    state_ptr->watch_for_handshake_start(chunk);
  });
}

std::unique_ptr<server_connection_iface> stdio_server_transport::accept(std::chrono::milliseconds /*timeout*/) {
  if (stopped_.load() || checked_out_.exchange(true))
    return nullptr;
  return std::make_unique<stdio_server_connection>(state_, [this] { checked_out_.store(false); });
}

void stdio_server_transport::stop() {
  stopped_.store(true);
  if (state_)
    state_->stop();
}

} // namespace bdg::bison::rmi::transport
