// MIT License © 2025 Binary Dice Games
/**
 * @file pty_client_transport_uv.cpp
 * @brief PTY client transport reimplemented using libuv — cross-platform.
 *
 * Replaces pty_client_transport_linux.cpp and pty_client_transport_win.cpp.
 * Drops the DCS+base64 protocol and uses the standard 4-byte big-endian length
 * prefix framing shared by all other transports.
 *
 * When stdin is a real TTY, it is opened with uv_tty_t in UV_TTY_MODE_RAW so
 * that binary frames pass through without line buffering or character
 * translation.  When stdin is a pipe (e.g. when this process is spawned by
 * pty_server_transport), uv_pipe_t is used instead — the fallback is
 * automatic via uv_guess_handle().
 *
 * Handshake: on open(), sends a HELLO frame, then waits for the server's
 * HELLO.  On shutdown(), sends an END frame.
 */
#include "src/rmi/transport/pty_client_transport.hpp"

#include <uv.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace bdg::bison::app {


// ── pty_client_transport::impl ────────────────────────────────────────────────

struct pty_client_transport::impl {
  uv_loop_t loop{};

  // Handles for stdin (read) and stdout (write).  One of each pair is active.
  uv_tty_t  stdin_tty{};
  uv_tty_t  stdout_tty{};
  uv_pipe_t stdin_pipe{};
  uv_pipe_t stdout_pipe{};

  uv_stream_t* read_stream{nullptr};   // points to active stdin handle
  uv_stream_t* write_stream{nullptr};  // points to active stdout handle
  bool         is_tty{false};

  uv_async_t send_async{};
  uv_async_t stop_async{};

  // Incremental frame parser — loop thread only.
  uint8_t      hdr[4]{};
  uint32_t     hdr_pos{0};
  uint32_t     payload_left{0};
  bison::buffer partial;
  std::vector<uint8_t> read_buf = std::vector<uint8_t>(65536);

  // Receive queue: loop → caller.
  std::mutex              recv_mtx;
  std::condition_variable recv_cv;
  std::queue<bison::buffer> recv_queue;
  std::atomic<bool>       recv_closed{false};

  // Receive queue for handshake (HELLO frame is consumed separately).
  std::atomic<bool> hello_received{false};

  // Send queue: caller → loop.
  std::mutex                       send_mtx;
  std::queue<std::vector<uint8_t>> send_queue;

  std::atomic<bool> opened{false};
  std::atomic<bool> stopped{false};
  std::thread       loop_thread;

  // ── libuv callbacks ─────────────────────────────────────────────────────────

  static void alloc_cb(uv_handle_t* h, size_t /*sug*/, uv_buf_t* buf) {
    auto* self = static_cast<impl*>(h->data);
    buf->base  = reinterpret_cast<char*>(self->read_buf.data());
    buf->len   = static_cast<decltype(buf->len)>(self->read_buf.size());
  }

  static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t*) {
    auto* self = static_cast<impl*>(stream->data);
    if (nread < 0) {
      {
        std::lock_guard<std::mutex> lk(self->recv_mtx);
        self->recv_closed.store(true);
      }
      self->recv_cv.notify_all();
      uv_read_stop(stream);
      return;
    }
    if (nread == 0) return;

    const auto* p    = self->read_buf.data();
    auto        left = static_cast<size_t>(nread);

    while (left > 0) {
      if (self->hdr_pos < 4) {
        const size_t take = std::min(size_t{4} - self->hdr_pos, left);
        std::memcpy(self->hdr + self->hdr_pos, p, take);
        self->hdr_pos += static_cast<uint32_t>(take);
        p    += take;
        left -= take;
        if (self->hdr_pos == 4) {
          uint32_t net_hdr; std::memcpy(&net_hdr, self->hdr, 4);
          self->payload_left = byte_swap(net_hdr);
          self->partial.clear();
          self->partial.reserve(self->payload_left);
        }
      }
      if (self->hdr_pos == 4 && (left > 0 || self->payload_left == 0)) {
        const size_t take =
            std::min(static_cast<size_t>(self->payload_left), left);
        self->partial.insert(self->partial.end(), p, p + take);
        self->payload_left -= static_cast<uint32_t>(take);
        p    += take;
        left -= take;
        if (self->payload_left == 0) {
          bison::buffer frame = std::move(self->partial);
          self->partial       = bison::buffer{};
          self->hdr_pos       = 0;

          // Handle HELLO frame specially; all others go to the data queue.
          static const bison::buffer kHello{'H','E','L','L','O'};
          if (!self->hello_received.load() && frame == kHello) {
            self->hello_received.store(true);
            self->recv_cv.notify_all();
          } else {
            {
              std::lock_guard<std::mutex> lk(self->recv_mtx);
              self->recv_queue.push(std::move(frame));
            }
            self->recv_cv.notify_one();
          }
        }
      }
    }
  }

  struct write_req { uv_write_t req{}; std::vector<uint8_t> data; };

  static void on_write_done(uv_write_t* req, int /*status*/) {
    delete reinterpret_cast<write_req*>(req);
  }

  static void on_send(uv_async_t* async) {
    auto* self = static_cast<impl*>(async->data);
    std::queue<std::vector<uint8_t>> q;
    {
      std::lock_guard<std::mutex> lk(self->send_mtx);
      std::swap(q, self->send_queue);
    }
    while (!q.empty()) {
      auto* wr = new write_req;
      wr->data = std::move(q.front());
      q.pop();
      uv_buf_t b = uv_buf_init(
          reinterpret_cast<char*>(wr->data.data()),
          static_cast<unsigned>(wr->data.size()));
      uv_write(&wr->req, self->write_stream, &b, 1, on_write_done);
    }
  }

  static void on_stop(uv_async_t* async) {
    auto* self = static_cast<impl*>(async->data);
    const auto close_if_active = [](uv_handle_t* h) {
      if (!uv_is_closing(h)) uv_close(h, nullptr);
    };
    close_if_active(reinterpret_cast<uv_handle_t*>(self->read_stream));
    if (self->write_stream != self->read_stream)
      close_if_active(reinterpret_cast<uv_handle_t*>(self->write_stream));
    close_if_active(reinterpret_cast<uv_handle_t*>(&self->send_async));
    close_if_active(reinterpret_cast<uv_handle_t*>(&self->stop_async));
  }

  // ── Frame helpers ──────────────────────────────────────────────────────────

  void enqueue_frame(const bison::buffer& frame) {
    std::vector<uint8_t> data(4 + frame.size());
    const uint32_t net_len = byte_swap(static_cast<uint32_t>(frame.size()));
    std::memcpy(data.data(), &net_len, 4);
    if (!frame.empty())
      std::memcpy(data.data() + 4, frame.data(), frame.size());
    {
      std::lock_guard<std::mutex> lk(send_mtx);
      send_queue.push(std::move(data));
    }
    uv_async_send(&send_async);
  }

  bool dequeue_frame(bison::buffer& frame, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(recv_mtx);
    if (!recv_cv.wait_for(lk, timeout, [this] {
          return !recv_queue.empty() || recv_closed.load();
        }))
      return false;
    if (recv_queue.empty()) return false;
    frame = std::move(recv_queue.front());
    recv_queue.pop();
    return true;
  }

  bool wait_for_hello(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(recv_mtx);
    return recv_cv.wait_for(lk, timeout, [this] {
      return hello_received.load() || recv_closed.load();
    });
  }

  // ── Loop start/stop ────────────────────────────────────────────────────────

  void start_loop() {
    loop_thread = std::thread([this] {
      uv_read_start(read_stream, alloc_cb, on_read);
      uv_run(&loop, UV_RUN_DEFAULT);
      {
        std::lock_guard<std::mutex> lk(recv_mtx);
        recv_closed.store(true);
      }
      recv_cv.notify_all();
      if (is_tty) uv_tty_reset_mode();
      uv_loop_close(&loop);
    });
  }

  void stop() {
    if (stopped.exchange(true)) return;
    uv_async_send(&stop_async);
    if (loop_thread.joinable()) loop_thread.join();
  }

  ~impl() { stop(); }
};

// ── pty_client_transport ──────────────────────────────────────────────────────

pty_client_transport::pty_client_transport()
    : impl_(std::make_unique<impl>()) {}

pty_client_transport::~pty_client_transport() { shutdown(); }

pty_client_transport::pty_client_transport(pty_client_transport&&) noexcept = default;
pty_client_transport& pty_client_transport::operator=(
    pty_client_transport&&) noexcept = default;

void pty_client_transport::open(bison::dynamic params) {
  if (!impl_) throw std::runtime_error("pty_client_transport::open: moved-from");
  if (impl_->opened.load()) return;

  auto& s = *impl_;

  int32_t handshake_timeout_ms = 300000;
  if (const auto* f = params.findField("handshake_timeout_ms"_key);
      f != nullptr && f->is<int32_t>())
    handshake_timeout_ms = f->as<int32_t>();

  uv_loop_init(&s.loop);

  const auto stdin_type = uv_guess_handle(0);
  s.is_tty = (stdin_type == UV_TTY);

  if (s.is_tty) {
    uv_tty_init(&s.loop, &s.stdin_tty, 0, 1);   // fd 0, readable
    uv_tty_init(&s.loop, &s.stdout_tty, 1, 0);  // fd 1, writable
    uv_tty_set_mode(&s.stdin_tty, UV_TTY_MODE_RAW);
    s.read_stream  = reinterpret_cast<uv_stream_t*>(&s.stdin_tty);
    s.write_stream = reinterpret_cast<uv_stream_t*>(&s.stdout_tty);
    s.stdin_tty.data  = &s;
    s.stdout_tty.data = &s;
  } else {
    uv_pipe_init(&s.loop, &s.stdin_pipe, 0);
    uv_pipe_open(&s.stdin_pipe, 0);
    uv_pipe_init(&s.loop, &s.stdout_pipe, 0);
    uv_pipe_open(&s.stdout_pipe, 1);
    s.read_stream  = reinterpret_cast<uv_stream_t*>(&s.stdin_pipe);
    s.write_stream = reinterpret_cast<uv_stream_t*>(&s.stdout_pipe);
    s.stdin_pipe.data  = &s;
    s.stdout_pipe.data = &s;
  }

  uv_async_init(&s.loop, &s.send_async, impl::on_send);
  s.send_async.data = &s;
  uv_async_init(&s.loop, &s.stop_async, impl::on_stop);
  s.stop_async.data = &s;

  s.start_loop();

  // Send HELLO and wait for server's HELLO.
  static const bison::buffer kHello{'H','E','L','L','O'};
  s.enqueue_frame(kHello);

  if (!s.wait_for_hello(std::chrono::milliseconds{handshake_timeout_ms}) ||
      !s.hello_received.load())
    throw std::runtime_error("pty_client_transport::open: handshake timeout");

  s.opened.store(true);
}

void pty_client_transport::send(bison::buffer frame) {
  if (!impl_ || !impl_->opened.load())
    throw std::runtime_error("pty_client_transport::send: not open");
  impl_->enqueue_frame(frame);
}

bool pty_client_transport::receive(bison::buffer& frame,
                                   std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->opened.load()) return false;
  return impl_->dequeue_frame(frame, timeout);
}

void pty_client_transport::shutdown() {
  if (!impl_ || !impl_->opened.load()) return;
  // Send END frame to signal the server.
  static const bison::buffer kEnd{'E','N','D'};
  impl_->enqueue_frame(kEnd);
  impl_->opened.store(false);
  impl_->stop();
}

} // namespace bdg::bison::app
