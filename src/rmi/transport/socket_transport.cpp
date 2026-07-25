// MIT License © 2025 Binary Dice Games
/**
 * @file socket_transport.cpp
 * @brief TCP socket transport implemented using libuv.
 *
 * Uses uv_tcp_t for all network I/O; each accepted connection runs its own
 * uv_loop_t on a background thread with a mutex-protected receive queue for
 * synchronous receive() calls. Accepted connections are handed off to their
 * own loop since uv_accept() requires the client handle to share the
 * listener's loop.
 *
 * Framing: 4-byte big-endian length prefix followed by payload.
 */
#include "src/rmi/transport/socket_transport.hpp"
#include "src/rmi/transport/uv_stream_state.hpp"

#include <uv.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#include <winsock2.h>
#else
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>

namespace bdg::bison::rmi::transport {

namespace {

// Duplicates the OS socket underlying an open uv_tcp_t handle. A plain
// `dup()` works for a POSIX socket fd but not for a Winsock `SOCKET`, which
// requires `WSADuplicateSocket`.
uv_os_sock_t duplicate_tcp_socket(uv_tcp_t* handle) {
  uv_os_fd_t fd{};

#if defined(_WIN32) || defined(__CYGWIN__)
  if (uv_fileno(reinterpret_cast<uv_handle_t*>(handle), &fd) != 0)
    return INVALID_SOCKET;

  // Winsock `SOCKET` handles aren't plain fds, so `dup()` doesn't apply.
  // `WSADuplicateSocketW()` produces a `WSAPROTOCOL_INFOW` blob describing
  // the socket, which `WSASocketW(..., FROM_PROTOCOL_INFO, ...)` turns into
  // a new socket handle in the target process — here, the same process,
  // since this is only used to hand a socket off from a temporary uv_tcp_t
  // to a fresh one on another loop.
  const SOCKET sock = reinterpret_cast<SOCKET>(fd);
  WSAPROTOCOL_INFOW info{};
  if (WSADuplicateSocketW(sock, GetCurrentProcessId(), &info) != 0)
    return INVALID_SOCKET;

  return WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0, 0);
#else
  if (uv_fileno(reinterpret_cast<uv_handle_t*>(handle), &fd) != 0)
    return -1;

  return dup(fd);
#endif
}

} // namespace

using tcp_conn_state = uv_stream_state<uv_tcp_t>;

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
  uv_loop_init(&st->loop);
  uv_tcp_init(&st->loop, &st->handle);

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

  st->init_asyncs();
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

  bison::synchronized<std::queue<std::unique_ptr<socket_server_connection>>> accept_queue;
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

    // uv_accept() requires the client handle to share the listener's loop,
    // but each connection runs on its own dedicated uv_loop_t/thread. Accept
    // into a temporary handle on the listener's loop, then hand a duplicate
    // of the underlying socket off to a fresh handle on the connection's own
    // loop via uv_tcp_open().
    uv_tcp_t temp{};
    uv_tcp_init(&im->accept_loop, &temp);
    if (uv_accept(server, reinterpret_cast<uv_stream_t*>(&temp)) != 0) {
      uv_close(reinterpret_cast<uv_handle_t*>(&temp), nullptr);
      return;
    }
    const uv_os_sock_t dup_sock = duplicate_tcp_socket(&temp);
    uv_close(reinterpret_cast<uv_handle_t*>(&temp), nullptr);
    if (dup_sock == static_cast<uv_os_sock_t>(-1))
      return;

    auto cst = std::make_unique<tcp_conn_state>();
    uv_loop_init(&cst->loop);
    uv_tcp_init(&cst->loop, &cst->handle);
    if (uv_tcp_open(&cst->handle, dup_sock) != 0)
      return;

    cst->init_asyncs();
    cst->start_loop();

    auto conn_impl = std::make_unique<socket_server_connection::impl>();
    conn_impl->st = std::move(cst);
    auto conn = std::make_unique<socket_server_connection>(std::move(conn_impl));

    im->accept_queue.withWLock([&](auto& q) { q.push(std::move(conn)); });
    im->accept_queue.notify_one();
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
  // Capture the impl pointer itself, not `this`: socket_server_transport is
  // movable, and a move after start() (e.g. returning one by value without
  // guaranteed NRVO) would leave this thread racing the moved-from object's
  // `impl_` member going null. `impl` is heap-owned and its address is
  // stable across moves of the owning socket_server_transport.
  auto* impl = impl_.get();
  impl_->accept_thread = std::thread([impl] {
    uv_run(&impl->accept_loop, UV_RUN_DEFAULT);
    uv_loop_close(&impl->accept_loop);
    // Wake any blocked accept() calls.
    impl->accept_queue.notify_all();
  });
}

socket_client_transport socket_server_transport::connect() const {
  return socket_client_transport{impl_->bind_host, impl_->port};
}

std::unique_ptr<server_connection_iface> socket_server_transport::accept(std::chrono::milliseconds timeout) {
  if (!impl_)
    return nullptr;
  if (!impl_->accept_queue.wait_for(timeout, [this](auto& q) { return !q.empty() || impl_->stopped.load(); }))
    return nullptr;
  return impl_->accept_queue.withWLock([](auto& q) -> std::unique_ptr<server_connection_iface> {
    if (q.empty())
      return nullptr;
    auto conn = std::move(q.front());
    q.pop();
    return conn;
  });
}

void socket_server_transport::stop() {
  if (!impl_ || !impl_->started.load() || impl_->stopped.exchange(true))
    return;
  uv_async_send(&impl_->stop_async);
  if (impl_->accept_thread.joinable())
    impl_->accept_thread.join();
  impl_->accept_queue.notify_all();
}

} // namespace bdg::bison::rmi::transport
