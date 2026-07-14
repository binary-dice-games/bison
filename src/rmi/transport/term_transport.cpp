// MIT License © 2025 Binary Dice Games
/**
 * @file term_transport.cpp
 * @brief libuv-backed implementation of the term transport, which frames
 *        client->server envelopes as OSC-99 sequences and server->client
 *        envelopes as `BISON<...>` markers (see term_role's doc comment for
 *        why the two directions need different wire formats).
 *        See term_transport.hpp and FORMAT.md §5.3 for the framing contract.
 */
#include "src/rmi/transport/term_transport.hpp"
#include "src/rmi/transport/uv_stream_state.hpp"

#include <gflags/gflags.h>
#include <uv.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace bdg::bison::rmi::transport {

namespace {

// Duplicates a plain file descriptor: `dup()` and the CRT's `_dup()` both
// take and return a plain `int`, so the platform difference is narrow
// enough to keep inline here.
int dup_fd(int fd) {
#if defined(_WIN32) || defined(__CYGWIN__)
  return _dup(fd);
#else
  return dup(fd);
#endif
}

// ── Base64 encoding/decoding ────────────────────────────────────────────────

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
          return std::nullopt;
        vals[j] = table[c - '+'];
      }
    }
    if (pad > 2 || (pad > 0 && i + 4 != in.size()))
      return std::nullopt;

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

bool parse_uint(std::string_view s, uint32_t& out) {
  if (s.empty() || s.size() > 10)
    return false;
  uint64_t v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9')
      return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
    if (v > 0xFFFFFFFFULL)
      return false;
  }
  out = static_cast<uint32_t>(v);
  return true;
}

// ── Wire role: which framing to use in which direction ──────────────────────
//
// ConPTY's two pipes are not symmetric the way a POSIX pty's master/slave
// is. Data flowing client-stdout -> ConPTY's *output* pipe -> server-read
// passes through ConPTY's output engine, which is built to shepherd OSC
// escape sequences through as atomic, opaque units (see the OSC-99 framing
// constants below and term_transport.hpp's doc comment) -- that direction
// is safe for OSC-99. Data flowing server-write -> ConPTY's *input* pipe ->
// client-stdin is not a passthrough channel: ConPTY runs it through a VT
// *input* state machine that only recognizes a small fixed table of real
// input sequences (arrow/function keys, mouse, bracketed paste, focus) to
// synthesize keyboard INPUT_RECORDs for the child process -- there is no
// "pass unrecognized bytes through verbatim" contract there, so an OSC-99
// sequence written into it is silently absorbed and never reaches the
// client as literal bytes. This exact asymmetry is documented in this
// repo's src/term/ANALYSIS.md §2, which uses plain literal text for the
// parent->child direction for the same reason. So: the client role sends
// OSC-99 (client-stdout direction) and receives the marker format
// (client-stdin direction); the server role does the reverse.
enum class term_role { client, server };

// ── OSC-99 framing constants (client -> server direction) ───────────────────

constexpr std::string_view kOscPrefix = "\x1b]99;";
constexpr char kOscTerminator = '\x07';

// Fixed per-sequence overhead ("\x1b]99;" + ';' + ';' + BEL) plus a generous
// budget for the decimal <seq>/<total> fields (10 digits each covers well
// beyond any envelope count this transport will ever see), subtracted from
// kMaxOscSequenceBytes to get how many base64 characters — and therefore how
// many raw payload bytes — one chunk may carry. See term_transport.hpp's
// doc comment for why this cap exists (ConPTY/pty relaying reliability).
constexpr size_t kOscFixedOverhead = kOscPrefix.size() + 1 + 1 + 1;
constexpr size_t kSeqDigitBudget = 10;
constexpr size_t kBase64Budget = kMaxOscSequenceBytes - kOscFixedOverhead - 2 * kSeqDigitBudget;
constexpr size_t kChunkRawBytes = (kBase64Budget / 4) * 3;
static_assert(kChunkRawBytes > 0, "kMaxOscSequenceBytes is too small to fit any chunk payload");

// ── Marker framing constants (server -> client direction) ───────────────────
//

constexpr std::string_view kMarkerPrefix = "BISON<";
constexpr char kMarkerSuffix = '>';

// Bound on how long a capturing-but-unterminated OSC-99 body is allowed to
// grow before it's treated as corrupt and discarded — well above what any
// legitimate chunk (bounded by kMaxOscSequenceBytes) could ever produce, so
// this only ever trips on a genuinely malformed/truncated sequence.
constexpr size_t kMaxCaptureBytes = kMaxOscSequenceBytes * 4;

} // namespace

// ── Default passthrough callbacks ───────────────────────────────────────────────

void term_print_passthrough(std::string_view chunk) {
  std::cout.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
  std::cout.flush();
}

void term_discard_passthrough(std::string_view /*chunk*/) {}

// ── term_pipe_thread: shared loop/thread lifecycle ──────────────────────────

/**
 * @brief Owns one libuv loop, one stream handle wrapping a duplicate of a
 *        caller-supplied fd, and its thread.
 *
 * The wrapped fd is usually a real pipe (a `terminal`'s ConPTY/pty I/O
 * handle), but when this process is itself running attached to a console
 * (e.g. a client launched directly inside the shell a `--transport=term`
 * server spawned), fd 0/1 are a console handle instead. `uv_pipe_open()`
 * only accepts real pipe/disk-file handles on native Windows — it rejects
 * console handles outright — so `uv_guess_handle()` picks the right libuv
 * stream constructor (`uv_pipe_open` vs `uv_tty_init`) at runtime. This is
 * not a platform branch: `uv_guess_handle()` returns the correct answer on
 * every platform, and POSIX's `uv_pipe_open()` already happens to accept
 * tty fds too, so this only changes behavior where the old code failed
 * outright (a Windows console handle).
 */
struct term_pipe_thread {
  uv_loop_t loop{};
  uv_any_handle handle{};
  uv_stream_t* stream{nullptr};
  uv_async_t stop_async{};
  std::atomic<bool> stopped{false};
  std::thread loop_thread;

  void init_pipe(int fd, const char* which, uv_async_cb on_stop, void* owner) {
    uv_loop_init(&loop);
    const int duped_fd = dup_fd(fd);
    if (duped_fd < 0)
      throw std::runtime_error(std::string{"term_transport: dup ("} + which + ") failed");

    const uv_handle_type guess = uv_guess_handle(duped_fd);

    if (guess == UV_TTY) {
      const bool readable = std::strcmp(which, "read") == 0;
      const int r = uv_tty_init(&loop, &handle.tty, duped_fd, readable);
      if (r != 0)
        throw std::runtime_error(std::string{"term_transport: uv_tty_init ("} + which + ") failed: " + uv_strerror(r));
#if defined(_WIN32)
      if (readable) {
        const int mode_r = uv_tty_set_mode(&handle.tty, UV_TTY_MODE_RAW);
        if (mode_r != 0)
          throw std::runtime_error(
              std::string{"term_transport: uv_tty_set_mode ("} + which + ") failed: " + uv_strerror(mode_r));
      }
#endif
      stream = reinterpret_cast<uv_stream_t*>(&handle.tty);
    } else {
      uv_pipe_init(&loop, &handle.pipe, 0);
      const int r = uv_pipe_open(&handle.pipe, duped_fd);
      if (r != 0)
        throw std::runtime_error(std::string{"term_transport: uv_pipe_open ("} + which + ") failed: " + uv_strerror(r));
      stream = reinterpret_cast<uv_stream_t*>(&handle.pipe);
    }
    stream->data = owner;
    uv_async_init(&loop, &stop_async, on_stop);
    stop_async.data = owner;
  }

  template <typename PreRun, typename PostRun>
  void start(PreRun&& pre_run, PostRun&& post_run) {
    loop_thread =
        std::thread([this, pre_run = std::forward<PreRun>(pre_run), post_run = std::forward<PostRun>(post_run)] {
          pre_run();
          uv_run(&loop, UV_RUN_DEFAULT);
          post_run();
          uv_loop_close(&loop);
        });
  }

  void stop() {
    if (stopped.exchange(true))
      return;
    if (!loop_thread.joinable())
      return;
    uv_async_send(&stop_async);
    if (std::this_thread::get_id() == loop_thread.get_id()) {
      // stop() was invoked synchronously from a callback running on this
      // very loop thread (e.g. a disconnect/EOF handler reached through
      // on_read() -> passthrough()) -- std::thread::join() on the thread
      // calling join() throws std::system_error("Resource deadlock
      // avoided"). uv_async_send() above already scheduled on_stop to run
      // and close every handle on the next loop iteration, so uv_run()
      // will return and the thread will finish on its own; detach instead
      // of joining so this call can return without waiting on itself.
      loop_thread.detach();
      return;
    }
    loop_thread.join();
  }
};

// ── Reader: OSC-99/marker scanner + chunk reassembly over one fd ────────────

struct term_reader {
  // See stdio_reader::kIdleFlushMs's doc comment — same rationale applies to
  // a partial match against prefix.
  static constexpr uint64_t kIdleFlushMs = 50;

  term_pipe_thread io;
  std::vector<uint8_t> read_buf = std::vector<uint8_t>(65536);
  uv_timer_t idle_timer{};

  // Which format this side expects to *receive* — the opposite of what its
  // own role sends, per the client/server table on term_role's doc comment.
  // Set once in init(), read-only afterwards.
  std::string_view prefix;
  char terminator{};
  bool osc_format{true};

  // Scanner state — loop thread only.
  bool in_seq{false};
  size_t match_pos{0};
  std::string speculative;
  std::string capture;
  // Bytes confirmed not to be part of a marker, held so a whole burst (e.g.
  // an entire VT escape sequence like arrow keys) reaches passthrough() as
  // one call instead of one call per byte -- see flush_pending()'s doc
  // comment for why per-byte delivery breaks downstream VT decoding.
  std::string pending_passthrough;
  // CSI-escape-sequence tracking (ESC '[' ... final-byte), independent of
  // the marker scan above. A real "BISON<...>" marker never legitimately
  // starts mid-CSI-sequence (markers and keystroke/VT passthrough are
  // independent producers sharing only the wire), so while esc_pending or
  // in_csi is set, feed_byte() bypasses marker-prefix matching entirely and
  // routes straight to pending_passthrough -- otherwise a CSI sequence whose
  // final byte happens to equal prefix[0] (e.g. Down arrow's "\x1b[B", where
  // 'B' == kMarkerPrefix[0]) gets its last byte held speculatively and
  // delayed by kIdleFlushMs, splitting the sequence exactly like the bug
  // pending_passthrough above already fixes for the non-colliding case.
  bool esc_pending{false};
  bool in_csi{false};

  // Chunk reassembly state — loop thread only.
  struct reassembly {
    bool active{false};
    uint32_t total{0};
    uint32_t expected_seq{0};
    bison::buffer buf;
  };
  reassembly assem;

  term_passthrough_cb passthrough;
  bison::synchronized<std::queue<bison::buffer>> recv_queue;
  std::atomic<bool> recv_closed{false};
  // Set the moment any frame is ever delivered to recv_queue; see
  // term_client_transport::is_connected()'s doc comment.
  std::atomic<bool> received_any{false};

  term_reader() = default;
  ~term_reader() {
    stop();
  }
  term_reader(const term_reader&) = delete;
  term_reader& operator=(const term_reader&) = delete;

  void init(int fd, term_passthrough_cb cb, term_role role) {
    passthrough = std::move(cb);
    // See term_role's doc comment: a side receives the format the *other*
    // side's role sends, not its own.
    osc_format = role == term_role::server;
    prefix = osc_format ? kOscPrefix : kMarkerPrefix;
    terminator = osc_format ? kOscTerminator : kMarkerSuffix;
    io.init_pipe(fd, "read", on_stop, this);
    uv_timer_init(&io.loop, &idle_timer);
    idle_timer.data = this;
  }

  void start_loop() {
    io.start(
        [this] { uv_read_start(io.stream, alloc_cb, on_read); },
        [this] {
          recv_closed.store(true);
          recv_queue.notify_all();
        });
  }

  void stop() {
    io.stop();
  }

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
    if (in_seq) {
      if (b == static_cast<uint8_t>(terminator)) {
        complete_sequence();
      } else {
        capture.push_back(static_cast<char>(b));
        // The size cap only makes sense for OSC-99 bodies (bounded by
        // kMaxOscSequenceBytes per chunk); marker bodies are un-chunked by
        // design (see kMarkerPrefix's doc comment) and may legitimately be
        // much larger than that.
        if (osc_format && capture.size() > kMaxCaptureBytes) {
          std::cerr << "[term_transport] warning: sequence exceeded max size, discarding\n";
          capture.clear();
          in_seq = false;
        }
      }
      return;
    }

    // While relaying an in-progress CSI escape sequence (ESC '[' ... final
    // byte), every byte -- even one that coincidentally equals prefix[0]
    // ('B') -- must reach passthrough as part of that same sequence. See
    // in_csi's doc comment for why: holding such a byte speculatively delays
    // it past kIdleFlushMs and tears the sequence apart downstream, exactly
    // like the bug pending_passthrough already fixes for the non-colliding
    // case (e.g. Down arrow's "\x1b[B").
    if (in_csi) {
      pending_passthrough.push_back(static_cast<char>(b));
      if (b >= 0x40 && b <= 0x7e)
        in_csi = false; // final byte -- sequence complete
      return;
    }

    if (esc_pending) {
      esc_pending = false;
      if (b == '[') {
        in_csi = true;
        pending_passthrough.push_back(static_cast<char>(b));
        return;
      }
      // Not a CSI introducer after all -- the previous ESC stood alone.
      // Fall through: b still needs full (marker/ESC/plain) handling below.
    }

    if (b == static_cast<uint8_t>(prefix[match_pos])) {
      speculative.push_back(static_cast<char>(b));
      ++match_pos;
      if (match_pos == prefix.size()) {
        in_seq = true;
        speculative.clear();
        capture.clear();
        match_pos = 0;
        uv_timer_stop(&idle_timer);
      } else {
        arm_idle_flush();
      }
      return;
    }

    // Mismatch: fold whatever was speculatively held into the pending
    // passthrough buffer (it turned out not to be a marker after all), then
    // reprocess this byte fresh (it may itself restart a match, or belong to
    // some other, unrelated escape sequence that must still reach the real
    // terminal).
    if (!speculative.empty()) {
      pending_passthrough += speculative;
      speculative.clear();
    }
    match_pos = 0;

    if (b == 0x1b) {
      // ESC can't itself start "BISON<" (prefix[0] is 'B'), but may be the
      // start of a CSI sequence -- see esc_pending's doc comment.
      esc_pending = true;
      uv_timer_stop(&idle_timer);
      pending_passthrough.push_back(static_cast<char>(b));
    } else if (b == static_cast<uint8_t>(prefix[0])) {
      match_pos = 1;
      speculative.push_back(static_cast<char>(b));
      arm_idle_flush();
    } else {
      uv_timer_stop(&idle_timer);
      pending_passthrough.push_back(static_cast<char>(b));
    }
  }

  void arm_idle_flush() {
    uv_timer_start(&idle_timer, on_idle_timeout, kIdleFlushMs, 0);
  }

  static void on_idle_timeout(uv_timer_t* h) {
    auto* self = static_cast<term_reader*>(h->data);
    if (!self->speculative.empty()) {
      self->pending_passthrough += self->speculative;
      self->speculative.clear();
    }
    self->match_pos = 0;
    self->flush_pending();
  }

  // Delivers everything accumulated in pending_passthrough as a single
  // passthrough() call. Bytes that are merely not part of a marker (the
  // common case for real keystrokes/VT sequences) are held here rather than
  // forwarded one at a time: passthrough's downstream consumer for the
  // anchor-terminal case (on_terminal_passthrough) writes straight into
  // another ConPTY's *input* pipe, which runs a VT input decoder with its
  // own escape-sequence timeout -- splitting e.g. an arrow key's 3-byte
  // "\x1b[A" across three separate writes lets that decoder time out mid
  // sequence and misread it as a lone Escape followed by literal '[' and
  // 'A'. Flushing once per on_read() call (see its call site) instead
  // reproduces however many bytes the OS actually delivered atomically.
  void flush_pending() {
    if (!pending_passthrough.empty()) {
      passthrough(pending_passthrough);
      pending_passthrough.clear();
    }
  }

  void complete_sequence() {
    if (osc_format)
      parse_and_complete(capture);
    else
      complete_marker(capture);
    capture.clear();
    in_seq = false;
    match_pos = 0;
    speculative.clear();
  }

  // ── Marker (BISON<base64>) completion: one frame per marker, no chunking —
  //    see kMarkerPrefix's doc comment. ────────────────────────────────────
  void complete_marker(const std::string& body) {
    auto decoded = base64_decode(body);
    if (!decoded) {
      std::cerr << "[term_transport] warning: malformed BISON<...> marker payload ignored\n";
      return;
    }
    recv_queue.withWLock([&](auto& q) { q.push(std::move(*decoded)); });
    received_any.store(true);
    recv_queue.notify_one();
  }

  void parse_and_complete(const std::string& body) {
    const auto first_semi = body.find(';');
    if (first_semi == std::string::npos) {
      std::cerr << "[term_transport] warning: malformed OSC-99 sequence ignored (no ';')\n";
      return;
    }
    const auto second_semi = body.find(';', first_semi + 1);
    if (second_semi == std::string::npos) {
      std::cerr << "[term_transport] warning: malformed OSC-99 sequence ignored (no second ';')\n";
      return;
    }
    uint32_t seq = 0;
    uint32_t total = 0;
    if (!parse_uint(std::string_view(body).substr(0, first_semi), seq) ||
        !parse_uint(std::string_view(body).substr(first_semi + 1, second_semi - first_semi - 1), total)) {
      std::cerr << "[term_transport] warning: malformed OSC-99 sequence ignored (bad seq/total)\n";
      return;
    }
    const std::string_view b64(body.data() + second_semi + 1, body.size() - second_semi - 1);
    complete_chunk(seq, total, b64);
  }

  void complete_chunk(uint32_t seq, uint32_t total, std::string_view b64_payload) {
    auto decoded = base64_decode(b64_payload);
    if (!decoded || total == 0) {
      std::cerr << "[term_transport] warning: malformed OSC-99 chunk payload ignored\n";
      return;
    }

    if (seq == 0) {
      if (assem.active && assem.expected_seq != assem.total) {
        std::cerr << "[term_transport] warning: stalled OSC-99 reassembly discarded (" << assem.expected_seq << "/"
                  << assem.total << ")\n";
      }
      assem = reassembly{};
      assem.active = true;
      assem.total = total;
    }

    if (!assem.active || seq != assem.expected_seq || total != assem.total) {
      std::cerr << "[term_transport] warning: out-of-sequence OSC-99 chunk discarded (seq=" << seq
                << ", expected=" << assem.expected_seq << ")\n";
      assem = reassembly{};
      return;
    }

    assem.buf.insert(assem.buf.end(), decoded->begin(), decoded->end());
    ++assem.expected_seq;
    if (assem.expected_seq == assem.total) {
      recv_queue.withWLock([&](auto& q) { q.push(std::move(assem.buf)); });
      received_any.store(true);
      recv_queue.notify_one();
      assem = reassembly{};
    }
  }

  // ── Static libuv callbacks ───────────────────────────────────────────────

  static void alloc_cb(uv_handle_t* h, size_t /*suggested*/, uv_buf_t* buf) {
    auto* self = static_cast<term_reader*>(h->data);
    buf->base = reinterpret_cast<char*>(self->read_buf.data());
    buf->len = static_cast<decltype(buf->len)>(self->read_buf.size());
  }

  static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* /*buf*/) {
    auto* self = static_cast<term_reader*>(stream->data);
    if (nread < 0) {
      self->recv_closed.store(true);
      self->recv_queue.notify_all();
      self->passthrough(std::string_view{}); // signals closure; see term_passthrough_cb's doc comment
      uv_read_stop(stream);
      return;
    }
    if (nread == 0)
      return;
    for (ssize_t i = 0; i < nread; ++i)
      self->feed_byte(self->read_buf[static_cast<size_t>(i)]);
    self->flush_pending();
  }

  static void on_stop(uv_async_t* h) {
    auto* self = static_cast<term_reader*>(h->data);
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(self->io.stream));
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(&self->io.stop_async));
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(&self->idle_timer));
  }
};

// ── Writer: raw byte sink over one fd ────────────────────────────────────────

struct term_writer {
  term_pipe_thread io;
  uv_async_t send_async{};

  bison::synchronized<std::queue<std::vector<uint8_t>>> send_queue;
  std::atomic<bool> stopped{false};

  term_writer() = default;
  ~term_writer() {
    stop();
  }
  term_writer(const term_writer&) = delete;
  term_writer& operator=(const term_writer&) = delete;

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
    auto* self = static_cast<term_writer*>(h->data);
    uv_flush_write_queue(self->send_queue, self->io.stream);
  }

  static void on_stop(uv_async_t* h) {
    auto* self = static_cast<term_writer*>(h->data);
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(self->io.stream));
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(&self->send_async));
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(&self->io.stop_async));
  }
};

// ── term_conn_state ──────────────────────────────────────────────────────────

struct term_conn_state {
  term_reader reader;
  term_writer writer;
  std::atomic<bool> closed{false};
  term_role role{term_role::client};

  void start(int read_fd, int write_fd, term_passthrough_cb passthrough, term_role r) {
    role = r;
    reader.init(read_fd, std::move(passthrough), role);
    writer.init(write_fd);
    reader.start_loop();
    writer.start_loop();
  }

  /**
   * @brief Encode @p frame for the wire and enqueue it as a single write.
   *        Client role: one or more `\x1b]99;<seq>;<total>;<base64>\x07`
   *        chunks (see kChunkRawBytes) — safe over the client-stdout ->
   *        ConPTY-output direction. Server role: a single un-chunked
   *        `BISON<base64>` marker — required over the server-write ->
   *        ConPTY-input direction (see term_role's doc comment).
   */
  void send_frame(const bison::buffer& frame) {
    if (closed.load())
      throw std::runtime_error("term_transport: send on closed connection");

    if (role == term_role::server) {
      std::string out;
      out.append(kMarkerPrefix);
      out.append(base64_encode(frame));
      out.push_back(kMarkerSuffix);
      writer.enqueue_bytes(std::vector<uint8_t>(out.begin(), out.end()));
      return;
    }

    const size_t total_chunks = frame.empty() ? 1 : (frame.size() + kChunkRawBytes - 1) / kChunkRawBytes;
    std::string out;
    for (size_t i = 0; i < total_chunks; ++i) {
      const size_t offset = i * kChunkRawBytes;
      const size_t len = std::min(kChunkRawBytes, frame.size() - offset);
      const bison::buffer chunk_bytes(
          frame.begin() + static_cast<ptrdiff_t>(offset), frame.begin() + static_cast<ptrdiff_t>(offset + len));
      out.append(kOscPrefix);
      out.append(std::to_string(i));
      out.push_back(';');
      out.append(std::to_string(total_chunks));
      out.push_back(';');
      out.append(base64_encode(chunk_bytes));
      out.push_back(kOscTerminator);
    }
    writer.enqueue_bytes(std::vector<uint8_t>(out.begin(), out.end()));
  }

  void send_raw(std::string_view bytes) {
    if (closed.load())
      return;
    writer.enqueue_bytes(std::vector<uint8_t>(bytes.begin(), bytes.end()));
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

// ── term_client_transport ────────────────────────────────────────────────────

term_client_transport::term_client_transport(
    int read_fd,
    int write_fd,
    term_passthrough_cb passthrough,
    std::chrono::milliseconds handshake_timeout,
    std::function<void()> before_destroy)
    : state_(std::make_unique<term_conn_state>()),
      handshake_timeout_(handshake_timeout),
      before_destroy_(std::move(before_destroy)) {
  state_->start(read_fd, write_fd, std::move(passthrough), term_role::client);
}

term_client_transport::~term_client_transport() {
  if (before_destroy_)
    before_destroy_();
  if (state_)
    state_->stop();
}

void term_client_transport::open(bison::dynamic /*params*/) {
  connect_deadline_ = std::chrono::steady_clock::now() + handshake_timeout_;
}

void term_client_transport::send(bison::buffer frame) {
  state_->send_frame(frame);
}

void term_client_transport::send(std::string_view bytes) {
  state_->send_raw(bytes);
}

bool term_client_transport::receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
  return state_->receive(frame, timeout);
}

void term_client_transport::shutdown() {
  // Run before_destroy_ here too (not just in the destructor): client::
  // disconnect() calls shutdown() well before this object is actually
  // destroyed, and state_->stop() below marks the connection closed, after
  // which send_raw() silently drops anything still buffered. Flushing here
  // means output produced right up to disconnect isn't lost. Safe to run
  // twice — the callback (scoped_terminal_config::stop_output_pump()) is
  // idempotent.
  if (before_destroy_)
    before_destroy_();
  state_->stop();
}

bool term_client_transport::is_connected() const {
  if (state_->reader.recv_closed.load())
    return false;
  if (state_->reader.received_any.load())
    return true;
  return std::chrono::steady_clock::now() < connect_deadline_;
}

// ── term_server_connection ───────────────────────────────────────────────────

term_server_connection::term_server_connection(std::shared_ptr<term_conn_state> state, std::function<void()> on_close)
    : state_(std::move(state)), on_close_(std::move(on_close)) {}

term_server_connection::~term_server_connection() {
  close();
}

void term_server_connection::send(bison::buffer frame) {
  // Mirrors socket_server_connection::send()'s closed check: once close()
  // has been called for this connection, refuse to write any more marker
  // bytes into the (possibly reused, per the class doc comment) underlying
  // pty -- otherwise a frame pushed by something that doesn't yet know this
  // connection is gone (e.g. a stray render update racing session teardown)
  // would still reach the wire, and on a real terminal a stray
  // `BISON<...>` marker is visible garbage rather than a silently dropped
  // byte.
  if (closed_.load())
    throw std::runtime_error("term_server_connection::send: closed");
  state_->send_frame(frame);
}

bool term_server_connection::receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
  if (closed_.load())
    return false;
  return state_->receive(frame, timeout);
}

void term_server_connection::close() {
  if (closed_.exchange(true))
    return;
  if (on_close_)
    on_close_();
}

bool term_server_connection::is_closed() const {
  return closed_.load();
}

// ── term_server_transport ─────────────────────────────────────────────────────

term_server_transport::term_server_transport(int read_fd, int write_fd, term_passthrough_cb passthrough)
    : read_fd_(read_fd), write_fd_(write_fd), passthrough_(std::move(passthrough)) {}

term_server_transport::~term_server_transport() {
  if (state_)
    state_->stop();
}

void term_server_transport::start(bison::dynamic /*params*/) {
  stopped_.store(false);
  state_ = std::make_shared<term_conn_state>();
  state_->start(read_fd_, write_fd_, passthrough_, term_role::server);
}

std::unique_ptr<server_connection_iface> term_server_transport::accept(std::chrono::milliseconds timeout) {
  if (!checked_out_.wait_for(timeout, [this](bool& co) { return !co || stopped_.load(); }))
    return nullptr;
  return checked_out_.withWLock([this](bool& co) -> std::unique_ptr<server_connection_iface> {
    if (stopped_.load() || co)
      return nullptr;
    co = true;
    return std::make_unique<term_server_connection>(state_, [this] {
      checked_out_.withWLock([](bool& c) { c = false; });
      checked_out_.notify_all();
    });
  });
}

void term_server_transport::stop() {
  stopped_.store(true);
  if (state_)
    state_->stop();
  checked_out_.notify_all();
}

} // namespace bdg::bison::rmi::transport
