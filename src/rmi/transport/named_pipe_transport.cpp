// MIT License © 2025 Binary Dice Games
/**
 * @file named_pipe_transport.cpp
 * @brief Named-pipe / Unix-socket transport reimplemented using libuv.
 *
 * Uses uv_pipe_t for all I/O; a single file covers both Windows named pipes
 * and Linux/macOS Unix-domain sockets transparently.
 *
 * Framing: 4-byte big-endian length prefix followed by payload.
 */
#include "src/rmi/transport/named_pipe_transport.hpp"
#include "src/rmi/transport/uv_stream_state.hpp"

#include <uv.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>

namespace bdg::bison::rmi::transport {

using pipe_conn_state = uv_stream_state<uv_pipe_t>;

// ── named_pipe_conn ────────────────────────────────────────────────────────────

/** @brief Connection-state bundle; one instance per open connection. */
struct named_pipe_conn {
  std::unique_ptr<pipe_conn_state> cs;
  bool closed{false};
};

// ── named_pipe_server_state ────────────────────────────────────────────────────

/** @brief Accept-loop state; owned by named_pipe_server_transport. */
struct named_pipe_server_state {
  uv_loop_t loop{};
  uv_pipe_t listener{};
  uv_async_t stop_async{};

  bison::synchronized<std::queue<std::unique_ptr<named_pipe_server_connection>>> accept_queue;
  std::condition_variable_any accept_cv;
  std::atomic<bool> stopped{false};
  std::thread loop_thread;

  static void on_stop(uv_async_t* async) {
    auto* ss = static_cast<named_pipe_server_state*>(async->data);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&ss->listener)))
      uv_close(reinterpret_cast<uv_handle_t*>(&ss->listener), nullptr);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&ss->stop_async)))
      uv_close(reinterpret_cast<uv_handle_t*>(&ss->stop_async), nullptr);
  }

  static void on_new_connection(uv_stream_t* server, int status);
};

// ── Forward declare so on_new_connection can construct it ─────────────────────

static std::unique_ptr<named_pipe_server_connection> make_server_connection(std::unique_ptr<named_pipe_conn> conn);

void named_pipe_server_state::on_new_connection(uv_stream_t* server, int status) {
  if (status < 0)
    return;
  auto* ss = static_cast<named_pipe_server_state*>(server->data);

  // Build connection state on its own loop.
  auto cs = std::make_unique<pipe_conn_state>();
  uv_loop_init(&cs->loop);
  uv_pipe_init(&cs->loop, &cs->handle, 0);

  if (uv_accept(server, reinterpret_cast<uv_stream_t*>(&cs->handle)) != 0)
    return;

  cs->init_asyncs();
  cs->start_loop();

  auto conn = std::make_unique<named_pipe_conn>();
  conn->cs = std::move(cs);
  auto wrapped = make_server_connection(std::move(conn));
  ss->accept_queue.withWLock([&](auto& q) { q.push(std::move(wrapped)); });
  ss->accept_cv.notify_one();
}

// ── named_pipe_server_connection ─────────────────────────────────────────────

named_pipe_server_connection::named_pipe_server_connection(std::unique_ptr<named_pipe_conn> conn)
    : conn_(std::move(conn)) {}

named_pipe_server_connection::~named_pipe_server_connection() {
  close();
}

static std::unique_ptr<named_pipe_server_connection> make_server_connection(std::unique_ptr<named_pipe_conn> conn) {
  return std::make_unique<named_pipe_server_connection>(std::move(conn));
}

void named_pipe_server_connection::send(bison::buffer frame) {
  if (!conn_ || conn_->closed)
    throw std::runtime_error("named_pipe_server_connection::send: closed");
  conn_->cs->enqueue_frame(frame);
}

bool named_pipe_server_connection::receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
  if (!conn_ || conn_->closed)
    return false;
  return conn_->cs->dequeue_frame(frame, timeout);
}

void named_pipe_server_connection::close() {
  if (!conn_ || conn_->closed)
    return;
  conn_->closed = true;
  conn_->cs->stop();
}

bool named_pipe_server_connection::is_closed() const {
  return !conn_ || conn_->closed;
}

// ── named_pipe_client_transport ───────────────────────────────────────────────

named_pipe_client_transport::named_pipe_client_transport(std::string path) : path_(std::move(path)) {}

named_pipe_client_transport::~named_pipe_client_transport() {
  shutdown();
}

void named_pipe_client_transport::open(bison::dynamic /*params*/) {
  if (conn_ && !conn_->closed)
    return; // already open

  auto cs = std::make_unique<pipe_conn_state>();
  uv_loop_init(&cs->loop);
  uv_pipe_init(&cs->loop, &cs->handle, 0);

  struct connect_ctx {
    bool done{false};
    int status{0};
  };
  connect_ctx ctx;
  cs->handle.data = &ctx;

  uv_connect_t connect_req{};
  connect_req.data = &ctx;
  uv_pipe_connect(&connect_req, &cs->handle, path_.c_str(), [](uv_connect_t* req, int status) {
    auto* c = static_cast<connect_ctx*>(req->data);
    c->status = status;
    c->done = true;
  });

  while (!ctx.done)
    uv_run(&cs->loop, UV_RUN_NOWAIT);

  if (ctx.status != 0)
    throw std::runtime_error(
        std::string{"named_pipe_client_transport::open: connect to '"} + path_ + "': " + uv_strerror(ctx.status));

  cs->init_asyncs();
  cs->start_loop();

  conn_ = std::make_unique<named_pipe_conn>();
  conn_->cs = std::move(cs);
  conn_->closed = false;
}

void named_pipe_client_transport::send(bison::buffer frame) {
  if (!conn_ || conn_->closed)
    throw std::runtime_error("named_pipe_client_transport::send: not open");
  conn_->cs->enqueue_frame(frame);
}

bool named_pipe_client_transport::receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
  if (!conn_ || conn_->closed)
    return false;
  return conn_->cs->dequeue_frame(frame, timeout);
}

void named_pipe_client_transport::shutdown() {
  if (!conn_ || conn_->closed)
    return;
  conn_->closed = true;
  conn_->cs->stop();
}

// ── named_pipe_server_transport ───────────────────────────────────────────────

named_pipe_server_transport::named_pipe_server_transport(std::string path) : path_(std::move(path)) {}

named_pipe_server_transport::~named_pipe_server_transport() {
  stop();
}

void named_pipe_server_transport::start(bison::dynamic /*params*/) {
  if (state_)
    return; // already started

  auto ss = std::make_unique<named_pipe_server_state>();
  uv_loop_init(&ss->loop);
  uv_pipe_init(&ss->loop, &ss->listener, 0);
  ss->listener.data = ss.get();
  uv_async_init(&ss->loop, &ss->stop_async, named_pipe_server_state::on_stop);
  ss->stop_async.data = ss.get();

  int r = uv_pipe_bind(&ss->listener, path_.c_str());
  if (r != 0)
    throw std::runtime_error(
        std::string{"named_pipe_server_transport::start: uv_pipe_bind '"} + path_ + "': " + uv_strerror(r));

  r = uv_listen(reinterpret_cast<uv_stream_t*>(&ss->listener), 128, named_pipe_server_state::on_new_connection);
  if (r != 0)
    throw std::runtime_error(std::string{"named_pipe_server_transport::start: uv_listen: "} + uv_strerror(r));

  stopped_.store(false);
  state_ = std::move(ss);
  state_->loop_thread = std::thread([this] {
    uv_run(&state_->loop, UV_RUN_DEFAULT);
    uv_loop_close(&state_->loop);
    state_->accept_cv.notify_all();
  });
}

std::unique_ptr<server_connection_iface> named_pipe_server_transport::accept(
    std::chrono::milliseconds timeout) {
  if (!state_)
    return nullptr;
  if (!state_->accept_queue.wait_for(state_->accept_cv, timeout,
                                     [this](auto& q) { return !q.empty() || stopped_.load(); }))
    return nullptr;
  return state_->accept_queue.withWLock(
      [](auto& q) -> std::unique_ptr<server_connection_iface> {
        if (q.empty())
          return nullptr;
        auto conn = std::move(q.front());
        q.pop();
        return conn;
      });
}

void named_pipe_server_transport::stop() {
  if (!state_ || stopped_.exchange(true))
    return;
  uv_async_send(&state_->stop_async);
  if (state_->loop_thread.joinable())
    state_->loop_thread.join();
  state_.reset();
}

} // namespace bdg::bison::rmi::transport
