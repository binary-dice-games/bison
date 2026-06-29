// MIT License © 2025 Binary Dice Games
/**
 * @file socket_transport_uv.cpp
 * @brief TCP socket transport reimplemented using libuv.
 *
 * Replaces the standalone-ASIO implementation in socket_transport.cpp.
 * Uses uv_tcp_t for all network I/O; each accepted connection runs its own
 * uv_loop_t on a background thread with a mutex-protected receive queue for
 * synchronous receive() calls.
 *
 * Framing: 4-byte big-endian length prefix followed by payload.
 */
#include "src/rmi/transport/socket_transport.hpp"

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

namespace bdg::bison::rmi::transport {

// ── Shared stream state (client connection or accepted server connection) ──────

struct tcp_conn_state {
  uv_loop_t loop{};
  uv_tcp_t handle{};
  uv_async_t send_async{};
  uv_async_t stop_async{};

  // Incremental frame parser — loop thread only.
  uint8_t hdr[4]{};
  uint32_t hdr_pos{0};
  uint32_t payload_left{0};
  bison::buffer partial;
  std::vector<uint8_t> read_buf = std::vector<uint8_t>(65536);

  // Receive queue: loop → caller.
  std::mutex recv_mtx;
  std::condition_variable recv_cv;
  std::queue<bison::buffer> recv_queue;
  std::atomic<bool> recv_closed{false};

  // Send queue: caller → loop.
  std::mutex send_mtx;
  std::queue<std::vector<uint8_t>> send_queue;

  std::atomic<bool> stopped{false};
  std::thread loop_thread;

  // ── Callbacks ─────────────────────────────────────────────────────────────

  static void alloc_cb(uv_handle_t* h, size_t /*sug*/, uv_buf_t* buf) {
    auto* st = static_cast<tcp_conn_state*>(h->data);
    buf->base = reinterpret_cast<char*>(st->read_buf.data());
    buf->len = static_cast<decltype(buf->len)>(st->read_buf.size());
  }

  static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* st = static_cast<tcp_conn_state*>(stream->data);
    if (nread < 0) {
      {
        std::lock_guard<std::mutex> lk(st->recv_mtx);
        st->recv_closed.store(true);
      }
      st->recv_cv.notify_all();
      uv_read_stop(stream);
      return;
    }
    if (nread == 0)
      return;

    const auto* p = reinterpret_cast<const uint8_t*>(buf->base);
    auto left = static_cast<size_t>(nread);

    while (left > 0) {
      if (st->hdr_pos < 4) {
        const size_t take = std::min(size_t{4} - st->hdr_pos, left);
        std::memcpy(st->hdr + st->hdr_pos, p, take);
        st->hdr_pos += static_cast<uint32_t>(take);
        p += take;
        left -= take;
        if (st->hdr_pos == 4) {
          uint32_t net_hdr;
          std::memcpy(&net_hdr, st->hdr, 4);
          st->payload_left = byte_swap(net_hdr);
          st->partial.clear();
          st->partial.reserve(st->payload_left);
        }
      }
      if (st->hdr_pos == 4 && (left > 0 || st->payload_left == 0)) {
        const size_t take = std::min(static_cast<size_t>(st->payload_left), left);
        st->partial.insert(st->partial.end(), p, p + take);
        st->payload_left -= static_cast<uint32_t>(take);
        p += take;
        left -= take;
        if (st->payload_left == 0) {
          {
            std::lock_guard<std::mutex> lk(st->recv_mtx);
            st->recv_queue.push(std::move(st->partial));
          }
          st->recv_cv.notify_one();
          st->partial = bison::buffer{};
          st->hdr_pos = 0;
        }
      }
    }
  }

  struct write_req {
    uv_write_t req{};
    std::vector<uint8_t> data;
  };

  static void on_write_done(uv_write_t* req, int /*status*/) {
    delete reinterpret_cast<write_req*>(req);
  }

  static void on_send(uv_async_t* async) {
    auto* st = static_cast<tcp_conn_state*>(async->data);
    std::queue<std::vector<uint8_t>> q;
    {
      std::lock_guard<std::mutex> lk(st->send_mtx);
      std::swap(q, st->send_queue);
    }
    while (!q.empty()) {
      auto* wr = new write_req;
      wr->data = std::move(q.front());
      q.pop();
      uv_buf_t b = uv_buf_init(reinterpret_cast<char*>(wr->data.data()), static_cast<unsigned>(wr->data.size()));
      uv_write(&wr->req, reinterpret_cast<uv_stream_t*>(&st->handle), &b, 1, on_write_done);
    }
  }

  static void on_stop(uv_async_t* async) {
    auto* st = static_cast<tcp_conn_state*>(async->data);
    const auto close_if_active = [](uv_handle_t* h) {
      if (!uv_is_closing(h))
        uv_close(h, nullptr);
    };
    close_if_active(reinterpret_cast<uv_handle_t*>(&st->handle));
    close_if_active(reinterpret_cast<uv_handle_t*>(&st->send_async));
    close_if_active(reinterpret_cast<uv_handle_t*>(&st->stop_async));
  }

  // ── Lifecycle ─────────────────────────────────────────────────────────────

  void init_handles() {
    uv_loop_init(&loop);
    uv_tcp_init(&loop, &handle);
    handle.data = this;
    uv_async_init(&loop, &send_async, on_send);
    send_async.data = this;
    uv_async_init(&loop, &stop_async, on_stop);
    stop_async.data = this;
  }

  void start_loop() {
    loop_thread = std::thread([this] {
      uv_read_start(reinterpret_cast<uv_stream_t*>(&handle), alloc_cb, on_read);
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
    if (stopped.exchange(true))
      return;
    uv_async_send(&stop_async);
    if (loop_thread.joinable())
      loop_thread.join();
  }

  ~tcp_conn_state() {
    stop();
  }

  // ── send / receive ─────────────────────────────────────────────────────────

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
    if (!recv_cv.wait_for(lk, timeout, [this] { return !recv_queue.empty() || recv_closed.load(); }))
      return false;
    if (recv_queue.empty())
      return false;
    frame = std::move(recv_queue.front());
    recv_queue.pop();
    return true;
  }
};

// ── socket_client_transport::impl ────────────────────────────────────────────

struct socket_client_transport::impl {
  std::string host;
  uint16_t port;
  std::unique_ptr<tcp_conn_state> st;
  bool opened{false};
};

// ── socket_client_transport ───────────────────────────────────────────────────

socket_client_transport::socket_client_transport(std::string host, uint16_t port) : impl_(std::make_unique<impl>()) {
  impl_->host = std::move(host);
  impl_->port = port;
}

socket_client_transport::~socket_client_transport() {
  shutdown();
}

socket_client_transport::socket_client_transport(socket_client_transport&&) noexcept = default;
socket_client_transport& socket_client_transport::operator=(socket_client_transport&&) noexcept = default;

void socket_client_transport::open(bison::dynamic params) {
  if (!impl_)
    throw std::runtime_error("socket_client_transport::open: moved-from");
  if (impl_->opened)
    return;

  // Allow params to override host/port.
  if (const auto* f = params.findField("host"_key); f != nullptr && f->is<std::string>())
    impl_->host = f->as<std::string>();
  if (const auto* f = params.findField("port"_key); f != nullptr && f->is<int32_t>())
    impl_->port = static_cast<uint16_t>(f->as<int32_t>());

  auto st = std::make_unique<tcp_conn_state>();
  st->init_handles();

  struct connect_ctx {
    tcp_conn_state* st;
    bool done{false};
    int status{0};
  };
  connect_ctx ctx{st.get()};
  st->handle.data = &ctx;

  // Resolve and connect synchronously using a temporary loop run.
  uv_getaddrinfo_t gai{};
  struct addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct gai_ctx {
    struct addrinfo* res{nullptr};
    bool done{false};
    int status{0};
  };
  gai_ctx gai_result;
  gai.data = &gai_result;

  int r = uv_getaddrinfo(
      &st->loop,
      &gai,
      [](uv_getaddrinfo_t* req, int status, struct addrinfo* res) {
        auto* ctx = static_cast<gai_ctx*>(req->data);
        ctx->status = status;
        ctx->res = res;
        ctx->done = true;
      },
      impl_->host.c_str(),
      std::to_string(impl_->port).c_str(),
      &hints);
  if (r != 0)
    throw std::runtime_error(std::string{"socket_client_transport::open: uv_getaddrinfo: "} + uv_strerror(r));

  while (!gai_result.done)
    uv_run(&st->loop, UV_RUN_NOWAIT);

  if (gai_result.status != 0 || !gai_result.res)
    throw std::runtime_error("socket_client_transport::open: DNS resolution failed");

  uv_connect_t connect_req{};
  connect_req.data = &ctx;
  r = uv_tcp_connect(&connect_req, &st->handle, gai_result.res->ai_addr, [](uv_connect_t* req, int status) {
    auto* c = static_cast<connect_ctx*>(req->data);
    c->status = status;
    c->done = true;
  });
  uv_freeaddrinfo(gai_result.res);

  if (r != 0)
    throw std::runtime_error(std::string{"socket_client_transport::open: uv_tcp_connect: "} + uv_strerror(r));

  while (!ctx.done)
    uv_run(&st->loop, UV_RUN_NOWAIT);

  if (ctx.status != 0)
    throw std::runtime_error(std::string{"socket_client_transport::open: connect failed: "} + uv_strerror(ctx.status));

  // Restore handle.data (was temporarily overridden for the connect callback).
  st->handle.data = st.get();
  st->start_loop();

  impl_->st = std::move(st);
  impl_->opened = true;
}

void socket_client_transport::send(bison::buffer frame) {
  if (!impl_ || !impl_->st)
    throw std::runtime_error("socket_client_transport::send: not open");
  impl_->st->enqueue_frame(frame);
}

bool socket_client_transport::receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->st)
    return false;
  return impl_->st->dequeue_frame(frame, timeout);
}

void socket_client_transport::shutdown() {
  if (!impl_ || !impl_->st)
    return;
  impl_->st->stop();
  impl_->opened = false;
}

bool socket_client_transport::is_connected() const {
  if (!impl_ || !impl_->st)
    return false;
  return !impl_->st->recv_closed.load();
}

// ── socket_server_connection::impl ────────────────────────────────────────────

struct socket_server_connection::impl {
  std::unique_ptr<tcp_conn_state> st;
  bool closed{false};
};

// ── socket_server_connection ──────────────────────────────────────────────────

socket_server_connection::socket_server_connection() : impl_(std::make_unique<impl>()) {}

socket_server_connection::~socket_server_connection() {
  close();
}

socket_server_connection::socket_server_connection(socket_server_connection&&) noexcept = default;
socket_server_connection& socket_server_connection::operator=(socket_server_connection&&) noexcept = default;

socket_server_connection::socket_server_connection(std::unique_ptr<impl> i) : impl_(std::move(i)) {}

void socket_server_connection::send(bison::buffer frame) {
  if (!impl_ || !impl_->st || impl_->closed)
    throw std::runtime_error("socket_server_connection::send: closed");
  impl_->st->enqueue_frame(frame);
}

bool socket_server_connection::receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->st || impl_->closed)
    return false;
  return impl_->st->dequeue_frame(frame, timeout);
}

void socket_server_connection::close() {
  if (!impl_ || impl_->closed)
    return;
  impl_->closed = true;
  if (impl_->st)
    impl_->st->stop();
}

bool socket_server_connection::is_closed() const {
  return !impl_ || impl_->closed;
}

// ── socket_server_transport::impl ─────────────────────────────────────────────

struct socket_server_transport::impl {
  std::string bind_host;
  uint16_t port;

  uv_loop_t accept_loop{};
  uv_tcp_t acceptor{};
  uv_async_t stop_async{};

  std::mutex accept_mtx;
  std::condition_variable accept_cv;
  std::queue<std::unique_ptr<socket_server_connection>> accept_queue;
  std::atomic<bool> started{false};
  std::atomic<bool> stopped{false};
  std::thread accept_thread;

  static void on_accept_stop(uv_async_t* async) {
    auto* im = static_cast<socket_server_transport::impl*>(async->data);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&im->acceptor)))
      uv_close(reinterpret_cast<uv_handle_t*>(&im->acceptor), nullptr);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&im->stop_async)))
      uv_close(reinterpret_cast<uv_handle_t*>(&im->stop_async), nullptr);
  }

  static void on_new_connection(uv_stream_t* server, int status) {
    if (status < 0)
      return;
    auto* im = static_cast<socket_server_transport::impl*>(server->data);

    // Build a fresh tcp_conn_state for this connection.
    auto cst = std::make_unique<tcp_conn_state>();
    cst->init_handles();

    if (uv_accept(server, reinterpret_cast<uv_stream_t*>(&cst->handle)) != 0)
      return;

    cst->start_loop();

    auto conn_impl = std::make_unique<socket_server_connection::impl>();
    conn_impl->st = std::move(cst);
    auto conn = std::make_unique<socket_server_connection>(std::move(conn_impl));

    {
      std::lock_guard<std::mutex> lk(im->accept_mtx);
      im->accept_queue.push(std::move(conn));
    }
    im->accept_cv.notify_one();
  }
};

// ── socket_server_transport ───────────────────────────────────────────────────

socket_server_transport::socket_server_transport(std::string bind_host, uint16_t port)
    : impl_(std::make_unique<impl>()) {
  impl_->bind_host = std::move(bind_host);
  impl_->port = port;
}

socket_server_transport::~socket_server_transport() {
  stop();
}

socket_server_transport::socket_server_transport(socket_server_transport&&) noexcept = default;
socket_server_transport& socket_server_transport::operator=(socket_server_transport&&) noexcept = default;

void socket_server_transport::start(bison::dynamic params) {
  if (!impl_)
    throw std::runtime_error("socket_server_transport::start: moved-from");

  if (const auto* f = params.findField("host"_key); f != nullptr && f->is<std::string>())
    impl_->bind_host = f->as<std::string>();
  if (const auto* f = params.findField("port"_key); f != nullptr && f->is<int32_t>())
    impl_->port = static_cast<uint16_t>(f->as<int32_t>());

  uv_loop_init(&impl_->accept_loop);
  uv_tcp_init(&impl_->accept_loop, &impl_->acceptor);
  impl_->acceptor.data = impl_.get();

  uv_async_init(&impl_->accept_loop, &impl_->stop_async, impl::on_accept_stop);
  impl_->stop_async.data = impl_.get();

  // Bind.
  struct sockaddr_storage addr{};
  int r = uv_ip4_addr(impl_->bind_host.c_str(), impl_->port, reinterpret_cast<struct sockaddr_in*>(&addr));
  if (r != 0)
    r = uv_ip6_addr(impl_->bind_host.c_str(), impl_->port, reinterpret_cast<struct sockaddr_in6*>(&addr));
  if (r != 0)
    throw std::runtime_error(std::string{"socket_server_transport::start: address parse failed: "} + impl_->bind_host);

  r = uv_tcp_bind(&impl_->acceptor, reinterpret_cast<const struct sockaddr*>(&addr), 0);
  if (r != 0)
    throw std::runtime_error(std::string{"socket_server_transport::start: uv_tcp_bind: "} + uv_strerror(r));

  r = uv_listen(reinterpret_cast<uv_stream_t*>(&impl_->acceptor), 128, impl::on_new_connection);
  if (r != 0)
    throw std::runtime_error(std::string{"socket_server_transport::start: uv_listen: "} + uv_strerror(r));

  impl_->stopped.store(false);
  impl_->started.store(true);
  impl_->accept_thread = std::thread([this] {
    uv_run(&impl_->accept_loop, UV_RUN_DEFAULT);
    uv_loop_close(&impl_->accept_loop);
    // Wake any blocked accept() calls.
    impl_->accept_cv.notify_all();
  });
}

socket_client_transport socket_server_transport::connect() const {
  return socket_client_transport{impl_->bind_host, impl_->port};
}

std::unique_ptr<server_connection_iface> socket_server_transport::accept(std::chrono::milliseconds timeout) {
  if (!impl_)
    return nullptr;
  std::unique_lock<std::mutex> lk(impl_->accept_mtx);
  if (!impl_->accept_cv.wait_for(lk, timeout, [this] { return !impl_->accept_queue.empty() || impl_->stopped.load(); }))
    return nullptr;
  if (impl_->accept_queue.empty())
    return nullptr;
  auto conn = std::move(impl_->accept_queue.front());
  impl_->accept_queue.pop();
  return conn;
}

void socket_server_transport::stop() {
  if (!impl_ || !impl_->started.load() || impl_->stopped.exchange(true))
    return;
  uv_async_send(&impl_->stop_async);
  if (impl_->accept_thread.joinable())
    impl_->accept_thread.join();
  impl_->accept_cv.notify_all();
}

} // namespace bdg::bison::rmi::transport
