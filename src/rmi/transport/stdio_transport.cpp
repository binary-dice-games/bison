// MIT License © 2025 Binary Dice Games
/**
 * @file stdio_transport.cpp
 * @brief libuv-backed implementation of the `BISON:` line-framed stdio
 *        transport. See stdio_transport.hpp and FORMAT.md for the framing
 *        contract.
 */
#include "src/rmi/transport/stdio_transport.hpp"

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

} // namespace

// ── Reader: BISON:-line scanner over one fd ────────────────────────────────────

/**
 * @brief Dedicated read-only libuv loop/thread for one fd.
 *
 * Runs a byte-level scanner (see stdio_transport.hpp's doc comment and
 * FORMAT.md) that forwards non-frame bytes to `passthrough` immediately and
 * enqueues decoded frames onto `recv_queue`.
 */
struct stdio_reader {
  uv_loop_t loop{};
  uv_pipe_t handle{};
  uv_async_t stop_async{};
  std::vector<uint8_t> read_buf = std::vector<uint8_t>(65536);

  // Scanner state — loop thread only.
  bool in_frame{false};
  size_t match_pos{1}; // pretend kFramePrefix[0] already matched at stream start
  std::string speculative;
  std::string payload;

  stdio_passthrough_cb passthrough;
  bison::synchronized<std::queue<bison::buffer>> recv_queue;
  std::atomic<bool> recv_closed{false};
  std::atomic<bool> stopped{false};
  std::thread loop_thread;

  stdio_reader() = default;
  ~stdio_reader() { stop(); }
  stdio_reader(const stdio_reader&) = delete;
  stdio_reader& operator=(const stdio_reader&) = delete;

  void init(int fd, stdio_passthrough_cb cb) {
    passthrough = std::move(cb);
    uv_loop_init(&loop);
    uv_pipe_init(&loop, &handle, 0);
    const int r = uv_pipe_open(&handle, fd);
    if (r != 0)
      throw std::runtime_error(std::string{"stdio_transport: uv_pipe_open (read) failed: "} + uv_strerror(r));
    handle.data = this;
    uv_async_init(&loop, &stop_async, on_stop);
    stop_async.data = this;
  }

  void start_loop() {
    loop_thread = std::thread([this] {
      uv_read_start(reinterpret_cast<uv_stream_t*>(&handle), alloc_cb, on_read);
      uv_run(&loop, UV_RUN_DEFAULT);
      recv_closed.store(true);
      recv_queue.notify_all();
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
    } else {
      const char c = static_cast<char>(b);
      passthrough(std::string_view{&c, 1});
    }
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
    auto close_if_active = [](uv_handle_t* handle) {
      if (!uv_is_closing(handle))
        uv_close(handle, nullptr);
    };
    close_if_active(reinterpret_cast<uv_handle_t*>(&self->handle));
    close_if_active(reinterpret_cast<uv_handle_t*>(&self->stop_async));
  }
};

// ── Writer: raw byte sink over one fd ───────────────────────────────────────────

/** @brief Dedicated write-only libuv loop/thread for one fd. */
struct stdio_writer {
  uv_loop_t loop{};
  uv_pipe_t handle{};
  uv_async_t send_async{};
  uv_async_t stop_async{};

  bison::synchronized<std::queue<std::vector<uint8_t>>> send_queue;
  std::atomic<bool> stopped{false};
  std::thread loop_thread;

  stdio_writer() = default;
  ~stdio_writer() { stop(); }
  stdio_writer(const stdio_writer&) = delete;
  stdio_writer& operator=(const stdio_writer&) = delete;

  void init(int fd) {
    uv_loop_init(&loop);
    uv_pipe_init(&loop, &handle, 0);
    const int r = uv_pipe_open(&handle, fd);
    if (r != 0)
      throw std::runtime_error(std::string{"stdio_transport: uv_pipe_open (write) failed: "} + uv_strerror(r));
    handle.data = this;
    uv_async_init(&loop, &send_async, on_send);
    send_async.data = this;
    uv_async_init(&loop, &stop_async, on_stop);
    stop_async.data = this;
  }

  void start_loop() {
    loop_thread = std::thread([this] {
      uv_run(&loop, UV_RUN_DEFAULT);
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

  void enqueue_bytes(std::vector<uint8_t> data) {
    if (stopped.load())
      return;
    send_queue.withWLock([&](auto& q) { q.push(std::move(data)); });
    uv_async_send(&send_async);
  }

  struct write_req {
    uv_write_t req{};
    std::vector<uint8_t> data;
  };

  static void on_send(uv_async_t* h) {
    auto* self = static_cast<stdio_writer*>(h->data);
    std::queue<std::vector<uint8_t>> pending;
    self->send_queue.withWLock([&](auto& q) { std::swap(pending, q); });

    while (!pending.empty()) {
      auto* wr = new write_req{};
      wr->data = std::move(pending.front());
      pending.pop();
      uv_buf_t buf = uv_buf_init(reinterpret_cast<char*>(wr->data.data()), static_cast<unsigned int>(wr->data.size()));
      wr->req.data = wr;
      uv_write(&wr->req, reinterpret_cast<uv_stream_t*>(&self->handle), &buf, 1, on_write_done);
    }
  }

  static void on_write_done(uv_write_t* req, int /*status*/) {
    delete static_cast<write_req*>(req->data);
  }

  static void on_stop(uv_async_t* h) {
    auto* self = static_cast<stdio_writer*>(h->data);
    auto close_if_active = [](uv_handle_t* handle) {
      if (!uv_is_closing(handle))
        uv_close(handle, nullptr);
    };
    close_if_active(reinterpret_cast<uv_handle_t*>(&self->handle));
    close_if_active(reinterpret_cast<uv_handle_t*>(&self->send_async));
    close_if_active(reinterpret_cast<uv_handle_t*>(&self->stop_async));
  }
};

// ── stdio_conn_state ────────────────────────────────────────────────────────────

struct stdio_conn_state {
  stdio_reader reader;
  stdio_writer writer;
  std::atomic<bool> closed{false};

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

stdio_client_transport::stdio_client_transport(int read_fd, int write_fd, stdio_passthrough_cb passthrough)
    : state_(std::make_unique<stdio_conn_state>()) {
  state_->start(read_fd, write_fd, std::move(passthrough));
}

stdio_client_transport::~stdio_client_transport() {
  if (state_)
    state_->stop();
}

void stdio_client_transport::open(bison::dynamic /*params*/) {}

void stdio_client_transport::send(bison::buffer frame) {
  state_->send_frame(frame);
}

bool stdio_client_transport::receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
  return state_->receive(frame, timeout);
}

void stdio_client_transport::shutdown() {
  state_->stop();
}

// ── stdio_server_connection ───────────────────────────────────────────────────

stdio_server_connection::stdio_server_connection(std::unique_ptr<stdio_conn_state> state) : state_(std::move(state)) {}

stdio_server_connection::~stdio_server_connection() {
  if (state_)
    state_->stop();
}

void stdio_server_connection::send(bison::buffer frame) {
  state_->send_frame(frame);
}

bool stdio_server_connection::receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
  return state_->receive(frame, timeout);
}

void stdio_server_connection::close() {
  state_->stop();
}

bool stdio_server_connection::is_closed() const {
  return state_->closed.load();
}

// ── stdio_server_transport ────────────────────────────────────────────────────

stdio_server_transport::stdio_server_transport(int read_fd, int write_fd, stdio_passthrough_cb passthrough)
    : read_fd_(read_fd), write_fd_(write_fd), passthrough_(std::move(passthrough)) {}

void stdio_server_transport::start(bison::dynamic /*params*/) {
  stopped_.store(false);
}

std::unique_ptr<server_connection_iface> stdio_server_transport::accept(std::chrono::milliseconds /*timeout*/) {
  if (stopped_.load() || accepted_.exchange(true))
    return nullptr;
  auto state = std::make_unique<stdio_conn_state>();
  state->start(read_fd_, write_fd_, passthrough_);
  return std::make_unique<stdio_server_connection>(std::move(state));
}

void stdio_server_transport::stop() {
  stopped_.store(true);
}

} // namespace bdg::bison::rmi::transport
