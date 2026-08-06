// MIT License © 2025 Binary Dice Games
/**
 * @file tls_socket_transport.cpp
 * @brief TLS-secured TCP socket transport implemented using libuv + mbedTLS.
 *
 * Structurally mirrors socket_transport.cpp: synchronous DNS+connect on the
 * client, an accept-time socket-duplication hand-off to a per-connection
 * uv_loop_t/thread on the server (see uv_stream_state.hpp / DESIGN.md §2).
 * The one addition is the mbedTLS handshake, layered in by tls_stream_state
 * (tls_stream_state.hpp) rather than duplicated here.
 *
 * Framing: 4-byte big-endian length prefix followed by payload, same as
 * socket_transport.cpp -- carried as TLS application data (FORMAT.md §5.1).
 */
#include "src/rmi/transport/tls_socket_transport.hpp"
#include "src/rmi/transport/tcp_socket_util.hpp"
#include "src/rmi/transport/tls_stream_state.hpp"

#include <uv.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>

namespace bdg::bison::rmi::transport {

using tls_conn_state = tls_stream_state<uv_tcp_t>;

namespace {

void read_string_field(const bison::dynamic& params, bison::hash_t key, std::string& out) {
  if (const auto* f = params.findField(key); f != nullptr && f->is<std::string>())
    out = f->as<std::string>();
}

void read_bool_field(const bison::dynamic& params, bison::hash_t key, bool& out) {
  if (const auto* f = params.findField(key); f != nullptr && f->is<bool>())
    out = f->as<bool>();
}

void read_client_tls_params(const bison::dynamic& params, tls_client_options& opts) {
  read_string_field(params, "server_name"_key, opts.server_name);
  read_string_field(params, "ca_file"_key, opts.ca_file);
  read_string_field(params, "ca_pem"_key, opts.ca_pem);
  read_bool_field(params, "insecure_skip_verify"_key, opts.insecure_skip_verify);
  read_string_field(params, "cert_file"_key, opts.cert_file);
  read_string_field(params, "cert_pem"_key, opts.cert_pem);
  read_string_field(params, "key_file"_key, opts.key_file);
  read_string_field(params, "key_pem"_key, opts.key_pem);
  read_string_field(params, "key_password"_key, opts.key_password);
}

void read_server_tls_params(const bison::dynamic& params, tls_server_options& opts) {
  read_string_field(params, "cert_file"_key, opts.cert_file);
  read_string_field(params, "cert_pem"_key, opts.cert_pem);
  read_string_field(params, "key_file"_key, opts.key_file);
  read_string_field(params, "key_pem"_key, opts.key_pem);
  read_string_field(params, "key_password"_key, opts.key_password);
  read_string_field(params, "ca_file"_key, opts.ca_file);
  read_string_field(params, "ca_pem"_key, opts.ca_pem);
  read_string_field(params, "client_auth"_key, opts.client_auth);
}

} // namespace

// ── tls_socket_client_transport::impl ────────────────────────────────────────

struct tls_socket_client_transport::impl {
  std::string host;
  uint16_t port;
  std::unique_ptr<tls_conn_state> st;
  bool opened{false};
};

// ── tls_socket_client_transport ───────────────────────────────────────────────

tls_socket_client_transport::tls_socket_client_transport(std::string host, uint16_t port)
    : impl_(std::make_unique<impl>()) {
  impl_->host = std::move(host);
  impl_->port = port;
}

tls_socket_client_transport::~tls_socket_client_transport() {
  shutdown();
}

tls_socket_client_transport::tls_socket_client_transport(tls_socket_client_transport&&) noexcept = default;
tls_socket_client_transport& tls_socket_client_transport::operator=(tls_socket_client_transport&&) noexcept = default;

void tls_socket_client_transport::open(bison::dynamic params) {
  if (!impl_)
    throw std::runtime_error("tls_socket_client_transport::open: moved-from");
  if (impl_->opened)
    return;

  // Allow params to override host/port, same as socket_client_transport.
  if (const auto* f = params.findField("host"_key); f != nullptr && f->is<std::string>())
    impl_->host = f->as<std::string>();
  if (const auto* f = params.findField("port"_key); f != nullptr && f->is<int32_t>())
    impl_->port = static_cast<uint16_t>(f->as<int32_t>());

  tls_client_options opts;
  opts.server_name = impl_->host;
  read_client_tls_params(params, opts);

  auto st = std::make_unique<tls_conn_state>();
  uv_loop_init(&st->io.loop);
  uv_tcp_init(&st->io.loop, &st->io.handle);

  struct connect_ctx {
    tls_conn_state* st;
    bool done{false};
    int status{0};
  };
  connect_ctx ctx{st.get()};
  st->io.handle.data = &ctx;

  // Resolve and connect synchronously using a temporary loop run, same
  // pattern as socket_client_transport::open().
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
      &st->io.loop,
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
    throw std::runtime_error(std::string{"tls_socket_client_transport::open: uv_getaddrinfo: "} + uv_strerror(r));

  while (!gai_result.done)
    uv_run(&st->io.loop, UV_RUN_NOWAIT);

  if (gai_result.status != 0 || !gai_result.res)
    throw std::runtime_error("tls_socket_client_transport::open: DNS resolution failed");

  uv_connect_t connect_req{};
  connect_req.data = &ctx;
  r = uv_tcp_connect(&connect_req, &st->io.handle, gai_result.res->ai_addr, [](uv_connect_t* req, int status) {
    auto* c = static_cast<connect_ctx*>(req->data);
    c->status = status;
    c->done = true;
  });
  uv_freeaddrinfo(gai_result.res);

  if (r != 0)
    throw std::runtime_error(std::string{"tls_socket_client_transport::open: uv_tcp_connect: "} + uv_strerror(r));

  while (!ctx.done)
    uv_run(&st->io.loop, UV_RUN_NOWAIT);

  if (ctx.status != 0)
    throw std::runtime_error(std::string{"tls_socket_client_transport::open: connect failed: "} +
                              uv_strerror(ctx.status));

  // TLS handshake runs synchronously on this (the caller's) thread, same as
  // the DNS/connect steps above; open() doesn't return until the connection
  // is fully usable. init_asyncs() is deliberately deferred until *after*
  // handshake_sync() succeeds -- see that method's doc comment for why.
  st->configure_as_client(opts);
  st->start_read();
  st->handshake_sync();
  st->init_asyncs();
  st->start_loop();

  impl_->st = std::move(st);
  impl_->opened = true;
}

void tls_socket_client_transport::send(bison::buffer frame) {
  if (!impl_ || !impl_->st)
    throw std::runtime_error("tls_socket_client_transport::send: not open");
  impl_->st->enqueue_frame(frame);
}

bool tls_socket_client_transport::receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->st)
    return false;
  return impl_->st->dequeue_frame(frame, timeout);
}

void tls_socket_client_transport::shutdown() {
  if (!impl_ || !impl_->st)
    return;
  impl_->st->stop();
  impl_->opened = false;
}

bool tls_socket_client_transport::is_connected() const {
  if (!impl_ || !impl_->st)
    return false;
  return !impl_->st->io.recv_closed.load();
}

// ── tls_socket_server_connection::impl ────────────────────────────────────────

struct tls_socket_server_connection::impl {
  std::unique_ptr<tls_conn_state> st;
  bool closed{false};
};

// ── tls_socket_server_connection ──────────────────────────────────────────────

tls_socket_server_connection::tls_socket_server_connection() : impl_(std::make_unique<impl>()) {}

tls_socket_server_connection::~tls_socket_server_connection() {
  close();
}

tls_socket_server_connection::tls_socket_server_connection(tls_socket_server_connection&&) noexcept = default;
tls_socket_server_connection& tls_socket_server_connection::operator=(tls_socket_server_connection&&) noexcept =
    default;

tls_socket_server_connection::tls_socket_server_connection(std::unique_ptr<impl> i) : impl_(std::move(i)) {}

void tls_socket_server_connection::send(bison::buffer frame) {
  if (!impl_ || !impl_->st || impl_->closed)
    throw std::runtime_error("tls_socket_server_connection::send: closed");
  impl_->st->enqueue_frame(frame);
}

bool tls_socket_server_connection::receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->st || impl_->closed)
    return false;
  return impl_->st->dequeue_frame(frame, timeout);
}

void tls_socket_server_connection::close() {
  if (!impl_ || impl_->closed)
    return;
  impl_->closed = true;
  if (impl_->st)
    impl_->st->stop();
}

bool tls_socket_server_connection::is_closed() const {
  return !impl_ || impl_->closed;
}

// ── tls_socket_server_transport::impl ─────────────────────────────────────────

struct tls_socket_server_transport::impl {
  std::string bind_host;
  uint16_t port;
  tls_server_options tls_opts;

  uv_loop_t accept_loop{};
  uv_tcp_t acceptor{};
  uv_async_t stop_async{};

  bison::synchronized<std::queue<std::unique_ptr<tls_socket_server_connection>>> accept_queue;
  std::atomic<bool> started{false};
  std::atomic<bool> stopped{false};
  std::thread accept_thread;

  static void on_accept_stop(uv_async_t* async) {
    auto* im = static_cast<tls_socket_server_transport::impl*>(async->data);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&im->acceptor)))
      uv_close(reinterpret_cast<uv_handle_t*>(&im->acceptor), nullptr);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&im->stop_async)))
      uv_close(reinterpret_cast<uv_handle_t*>(&im->stop_async), nullptr);
  }

  static void on_new_connection(uv_stream_t* server, int status) {
    if (status < 0)
      return;
    auto* im = static_cast<tls_socket_server_transport::impl*>(server->data);

    // Same accept-time hand-off as socket_transport.cpp's on_new_connection:
    // uv_accept() requires the client handle to share the listener's loop,
    // so accept into a temporary handle here, then duplicate the underlying
    // socket onto a fresh handle on the connection's own dedicated loop.
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

    auto cst = std::make_unique<tls_conn_state>();
    uv_loop_init(&cst->io.loop);
    uv_tcp_init(&cst->io.loop, &cst->io.handle);
    if (uv_tcp_open(&cst->io.handle, dup_sock) != 0)
      return;

    try {
      cst->configure_as_server(im->tls_opts);
    } catch (const std::exception&) {
      // Bad/unloadable server cert-key config: nothing a per-connection
      // handshake retry could fix, and every future connection would fail
      // identically, but the accept loop itself should keep running rather
      // than take the whole listener down over one connection's setup.
      return;
    }
    cst->init_asyncs();
    // Handshake runs on this connection's own dedicated thread, not here on
    // the shared accept thread -- see start_loop_with_handshake()'s comment.
    cst->start_loop_with_handshake();

    auto conn_impl = std::make_unique<tls_socket_server_connection::impl>();
    conn_impl->st = std::move(cst);
    auto conn = std::make_unique<tls_socket_server_connection>(std::move(conn_impl));

    im->accept_queue.withWLock([&](auto& q) { q.push(std::move(conn)); });
    im->accept_queue.notify_one();
  }
};

// ── tls_socket_server_transport ───────────────────────────────────────────────

tls_socket_server_transport::tls_socket_server_transport(std::string bind_host, uint16_t port)
    : impl_(std::make_unique<impl>()) {
  impl_->bind_host = std::move(bind_host);
  impl_->port = port;
}

tls_socket_server_transport::~tls_socket_server_transport() {
  stop();
}

tls_socket_server_transport::tls_socket_server_transport(tls_socket_server_transport&&) noexcept = default;
tls_socket_server_transport& tls_socket_server_transport::operator=(tls_socket_server_transport&&) noexcept =
    default;

void tls_socket_server_transport::start(bison::dynamic params) {
  if (!impl_)
    throw std::runtime_error("tls_socket_server_transport::start: moved-from");

  if (const auto* f = params.findField("host"_key); f != nullptr && f->is<std::string>())
    impl_->bind_host = f->as<std::string>();
  if (const auto* f = params.findField("port"_key); f != nullptr && f->is<int32_t>())
    impl_->port = static_cast<uint16_t>(f->as<int32_t>());
  read_server_tls_params(params, impl_->tls_opts);

  // Fail fast on an unusable cert/key configuration rather than accepting
  // connections that can only ever fail their handshake.
  validate_server_options(impl_->tls_opts);

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
    throw std::runtime_error(std::string{"tls_socket_server_transport::start: address parse failed: "} +
                              impl_->bind_host);

  r = uv_tcp_bind(&impl_->acceptor, reinterpret_cast<const struct sockaddr*>(&addr), 0);
  if (r != 0)
    throw std::runtime_error(std::string{"tls_socket_server_transport::start: uv_tcp_bind: "} + uv_strerror(r));

  r = uv_listen(reinterpret_cast<uv_stream_t*>(&impl_->acceptor), 128, impl::on_new_connection);
  if (r != 0)
    throw std::runtime_error(std::string{"tls_socket_server_transport::start: uv_listen: "} + uv_strerror(r));

  impl_->stopped.store(false);
  impl_->started.store(true);
  // Capture the impl pointer itself, not `this` -- see socket_transport.cpp's
  // identical comment on socket_server_transport::start() for why.
  auto* impl = impl_.get();
  impl_->accept_thread = std::thread([impl] {
    uv_run(&impl->accept_loop, UV_RUN_DEFAULT);
    uv_loop_close(&impl->accept_loop);
    impl->accept_queue.notify_all();
  });
}

tls_socket_client_transport tls_socket_server_transport::connect() const {
  return tls_socket_client_transport{impl_->bind_host, impl_->port};
}

std::unique_ptr<server_connection_iface> tls_socket_server_transport::accept(std::chrono::milliseconds timeout) {
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

void tls_socket_server_transport::stop() {
  if (!impl_ || !impl_->started.load() || impl_->stopped.exchange(true))
    return;
  uv_async_send(&impl_->stop_async);
  if (impl_->accept_thread.joinable())
    impl_->accept_thread.join();
  impl_->accept_queue.notify_all();
}

} // namespace bdg::bison::rmi::transport
