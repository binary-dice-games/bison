// MIT License © 2025 Binary Dice Games
/**
 * @file pty_server_transport_uv.cpp
 * @brief PTY server transport reimplemented using libuv — cross-platform.
 *
 * Replaces pty_server_transport_linux.cpp and pty_server_transport_win.cpp.
 * Spawns the child process with uv_spawn() and connects via uv_pipe_t for
 * its stdin and stdout.  Drops the DCS+base64 protocol and the forkpty/ConPTY
 * dependency in favour of the standard 4-byte big-endian length-prefix framing
 * used by all other transports.
 *
 * Handshake: accept() waits for a HELLO frame from the child's stdout, then
 * sends a HELLO frame to the child's stdin.  The child's pty_client_transport
 * mirrors this sequence in open().
 */
#include "src/rmi/transport/pty_server_transport.hpp"

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


// ── Shared state between server_transport and server_connection ────────────────

struct pty_server_state {
  uv_loop_t    loop{};
  uv_pipe_t    child_stdin{};   // server writes → child reads
  uv_pipe_t    child_stdout{};  // child writes → server reads
  uv_process_t process{};
  uv_async_t   send_async{};
  uv_async_t   stop_async{};

  // Incremental frame parser for child_stdout — loop thread only.
  uint8_t      hdr[4]{};
  uint32_t     hdr_pos{0};
  uint32_t     payload_left{0};
  bison::buffer partial;
  std::vector<uint8_t> read_buf = std::vector<uint8_t>(65536);

  // Receive queue: loop → pty_server_connection::receive().
  std::mutex              recv_mtx;
  std::condition_variable recv_cv;
  std::queue<bison::buffer> recv_queue;
  std::atomic<bool>       recv_closed{false};

  // Send queue: pty_server_connection::send() → child_stdin.
  std::mutex                       send_mtx;
  std::queue<std::vector<uint8_t>> send_queue;

  // Process lifecycle.
  std::atomic<bool>       process_alive{false};
  std::mutex              process_mtx;
  std::condition_variable process_cv;

  // Handshake state.
  std::atomic<bool> hello_received{false};

  // Active-session state.
  std::atomic<bool> session_accepted{false};
  std::atomic<bool> session_closed{false};
  std::mutex        session_mtx;
  std::condition_variable session_cv;

  std::string shell_cmd;
  std::thread loop_thread;
  std::atomic<bool> stopped{false};

  // ── libuv callbacks ─────────────────────────────────────────────────────────

  static void alloc_cb(uv_handle_t* h, size_t /*sug*/, uv_buf_t* buf) {
    auto* self = static_cast<pty_server_state*>(h->data);
    buf->base  = reinterpret_cast<char*>(self->read_buf.data());
    buf->len   = static_cast<decltype(buf->len)>(self->read_buf.size());
  }

  static void on_child_read(uv_stream_t* stream, ssize_t nread,
                             const uv_buf_t*) {
    auto* self = static_cast<pty_server_state*>(stream->data);
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

          static const bison::buffer kHello{'H','E','L','L','O'};
          static const bison::buffer kEnd{'E','N','D'};

          if (!self->hello_received.load() && frame == kHello) {
            // Handshake: first HELLO from child.
            self->hello_received.store(true);
            self->recv_cv.notify_all();
          } else if (frame == kEnd) {
            // Child is shutting down.
            {
              std::lock_guard<std::mutex> lk(self->session_mtx);
              self->session_closed.store(true);
            }
            self->session_cv.notify_all();
          } else {
            // Normal data frame.
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
    auto* self = static_cast<pty_server_state*>(async->data);
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
      uv_write(&wr->req,
               reinterpret_cast<uv_stream_t*>(&self->child_stdin),
               &b, 1, on_write_done);
    }
  }

  static void on_stop(uv_async_t* async) {
    auto* self = static_cast<pty_server_state*>(async->data);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&self->child_stdout)))
      uv_close(reinterpret_cast<uv_handle_t*>(&self->child_stdout), nullptr);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&self->child_stdin)))
      uv_close(reinterpret_cast<uv_handle_t*>(&self->child_stdin), nullptr);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&self->process)))
      uv_close(reinterpret_cast<uv_handle_t*>(&self->process), nullptr);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&self->send_async)))
      uv_close(reinterpret_cast<uv_handle_t*>(&self->send_async), nullptr);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&self->stop_async)))
      uv_close(reinterpret_cast<uv_handle_t*>(&self->stop_async), nullptr);
  }

  static void on_exit(uv_process_t* proc, int64_t /*exit_status*/,
                      int /*term_signal*/) {
    auto* self = static_cast<pty_server_state*>(proc->data);
    self->process_alive.store(false);
    {
      std::lock_guard<std::mutex> lk(self->session_mtx);
      self->session_closed.store(true);
    }
    self->process_cv.notify_all();
    self->session_cv.notify_all();
    self->recv_cv.notify_all();
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
          return !recv_queue.empty() || recv_closed.load() ||
                 session_closed.load();
        }))
      return false;
    if (recv_queue.empty()) return false;
    frame = std::move(recv_queue.front());
    recv_queue.pop();
    return true;
  }

  // ── Lifecycle ─────────────────────────────────────────────────────────────

  void start_loop() {
    loop_thread = std::thread([this] {
      uv_read_start(
          reinterpret_cast<uv_stream_t*>(&child_stdout), alloc_cb,
          on_child_read);
      uv_run(&loop, UV_RUN_DEFAULT);
      {
        std::lock_guard<std::mutex> lk(recv_mtx);
        recv_closed.store(true);
      }
      recv_cv.notify_all();
      uv_loop_close(&loop);
    });
  }

  void stop() {
    if (stopped.exchange(true)) return;
    // Kill the child if still running.
    if (process_alive.load()) {
      uv_process_kill(&process, /* SIGTERM */ 15);
    }
    uv_async_send(&stop_async);
    if (loop_thread.joinable()) loop_thread.join();
  }

  ~pty_server_state() { stop(); }

  void reset_session() {
    std::lock_guard<std::mutex> rlk(recv_mtx);
    while (!recv_queue.empty()) recv_queue.pop();
    recv_closed.store(false);
    hello_received.store(false);
    session_accepted.store(false);
    session_closed.store(false);
  }
};

// ── pty_server_connection::impl ───────────────────────────────────────────────

struct pty_server_connection::impl {
  std::shared_ptr<pty_server_state> srv;
  bool closed{false};
};

// ── pty_server_transport::impl ────────────────────────────────────────────────

struct pty_server_transport::impl {
  std::shared_ptr<pty_server_state> state;
  std::string shell;
};

// ── pty_server_connection ─────────────────────────────────────────────────────

pty_server_connection::pty_server_connection(
    std::unique_ptr<pty_server_connection::impl> i)
    : impl_(std::move(i)) {}

pty_server_connection::~pty_server_connection() { close(); }

pty_server_connection::pty_server_connection(
    pty_server_connection&&) noexcept = default;
pty_server_connection& pty_server_connection::operator=(
    pty_server_connection&&) noexcept = default;

void pty_server_connection::send(bison::buffer frame) {
  if (!impl_ || impl_->closed)
    throw std::runtime_error("pty_server_connection::send: closed");
  impl_->srv->enqueue_frame(frame);
}

bool pty_server_connection::receive(bison::buffer& frame,
                                    std::chrono::milliseconds timeout) {
  if (!impl_ || impl_->closed) return false;
  return impl_->srv->dequeue_frame(frame, timeout);
}

void pty_server_connection::close() {
  if (!impl_ || impl_->closed) return;
  impl_->closed = true;
  {
    std::lock_guard<std::mutex> lk(impl_->srv->session_mtx);
    impl_->srv->session_closed.store(true);
  }
  impl_->srv->session_cv.notify_all();
  impl_->srv->recv_cv.notify_all();
}

bool pty_server_connection::is_closed() const {
  return !impl_ || impl_->closed ||
         (impl_->srv && impl_->srv->session_closed.load());
}

// ── pty_server_transport ──────────────────────────────────────────────────────

pty_server_transport::pty_server_transport(std::string shell)
    : impl_(std::make_unique<pty_server_transport::impl>()) {
  impl_->shell = std::move(shell);
}

pty_server_transport::~pty_server_transport() { stop(); }

void pty_server_transport::start(bison::dynamic /*params*/) {
  if (!impl_) throw std::runtime_error("pty_server_transport::start: moved-from");
  if (impl_->state && impl_->state->process_alive.load()) return;  // already running

  auto state       = std::make_shared<pty_server_state>();
  state->shell_cmd = impl_->shell;

  uv_loop_init(&state->loop);
  uv_pipe_init(&state->loop, &state->child_stdin, 0);
  uv_pipe_init(&state->loop, &state->child_stdout, 0);
  state->child_stdout.data = state.get();
  state->child_stdin.data  = state.get();

  uv_async_init(&state->loop, &state->send_async, pty_server_state::on_send);
  state->send_async.data = state.get();
  uv_async_init(&state->loop, &state->stop_async, pty_server_state::on_stop);
  state->stop_async.data = state.get();

  // Configure child stdio: pipe for stdin[0] and stdout[1], inherit stderr[2].
  uv_stdio_container_t stdio[3]{};
  stdio[0].flags        = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_READABLE_PIPE);
  stdio[0].data.stream  = reinterpret_cast<uv_stream_t*>(&state->child_stdin);
  stdio[1].flags        = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
  stdio[1].data.stream  = reinterpret_cast<uv_stream_t*>(&state->child_stdout);
  stdio[2].flags        = UV_INHERIT_FD;
  stdio[2].data.fd      = 2;

  // Split shell_cmd into argv for uv_spawn.
  std::vector<std::string> args_storage;
  std::vector<char*>       argv;
  {
    const auto& cmd = state->shell_cmd;
    std::string tok;
    for (size_t i = 0; i <= cmd.size(); ++i) {
      if (i == cmd.size() || cmd[i] == ' ') {
        if (!tok.empty()) { args_storage.push_back(tok); tok.clear(); }
      } else {
        tok += cmd[i];
      }
    }
    for (auto& s : args_storage) argv.push_back(s.data());
    argv.push_back(nullptr);
  }

  uv_process_options_t opts{};
  opts.file       = argv[0];
  opts.args       = argv.data();
  opts.stdio      = stdio;
  opts.stdio_count = 3;
  opts.exit_cb    = pty_server_state::on_exit;

  state->process.data = state.get();
  int r = uv_spawn(&state->loop, &state->process, &opts);
  if (r != 0)
    throw std::runtime_error(
        std::string{"pty_server_transport::start: uv_spawn '"} +
        state->shell_cmd + "': " + uv_strerror(r));

  state->process_alive.store(true);
  state->start_loop();
  impl_->state = std::move(state);
}

std::unique_ptr<rmi::transport::server_connection_iface>
pty_server_transport::accept(std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->state) return nullptr;
  auto& s = *impl_->state;

  if (s.session_accepted.load()) return nullptr;  // one session at a time

  // Wait for HELLO from child.
  {
    std::unique_lock<std::mutex> lk(s.recv_mtx);
    if (!s.recv_cv.wait_for(lk, timeout, [&s] {
          return s.hello_received.load() || !s.process_alive.load();
        }))
      return nullptr;
  }
  if (!s.hello_received.load()) return nullptr;

  // Respond with HELLO.
  static const bison::buffer kHello{'H','E','L','L','O'};
  s.enqueue_frame(kHello);

  s.session_accepted.store(true);

  auto conn_impl  = std::make_unique<pty_server_connection::impl>();
  conn_impl->srv  = impl_->state;
  return std::make_unique<pty_server_connection>(std::move(conn_impl));
}

void pty_server_transport::stop() {
  if (!impl_ || !impl_->state) return;
  impl_->state->stop();
  impl_->state.reset();
}

void pty_server_transport::restart_session() {
  if (!impl_ || !impl_->state) return;
  impl_->state->reset_session();
}

bool pty_server_transport::is_shell_running() const {
  return impl_ && impl_->state && impl_->state->process_alive.load();
}

bool pty_server_transport::wait_until_closed(
    std::chrono::milliseconds timeout) const {
  if (!impl_ || !impl_->state) return true;
  auto& s = *impl_->state;
  std::unique_lock<std::mutex> lk(s.session_mtx);
  return s.session_cv.wait_for(lk, timeout, [&s] {
    return s.session_closed.load() || !s.process_alive.load();
  });
}

} // namespace bdg::bison::app
