// MIT License © 2025 Binary Dice Games
/**
 * @file pipe_transport.cpp
 * @brief Anonymous-pipe transport implemented using libuv.
 *
 * Establishes a full-duplex channel between in-process endpoints using a
 * pair of connected uv_pipe_t handles.  Both ends own a uv_loop_t that runs
 * on a dedicated background thread and bridges incoming frames into a
 * mutex-protected queue for synchronous receive() calls.
 *
 * Framing: 4-byte big-endian length prefix followed by payload, identical
 * to socket_transport and named_pipe_transport.
 */
#include "src/rmi/transport/pipe_transport.hpp"
#include "src/rmi/transport/uv_stream_state.hpp"

#include <uv.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>

namespace bdg::bison::rmi::transport {

// ── Unique path per channel ────────────────────────────────────────────────────

static std::string make_anon_pipe_path() {
  static std::atomic<uint64_t> counter{0};
  const auto id = counter.fetch_add(1, std::memory_order_relaxed);
  return std::string{"/tmp/.bison-anon-"} + std::to_string(id) + ".sock";
}

// ── Per-connection state ───────────────────────────────────────────────────────

using pipe_end_state = uv_stream_state<uv_pipe_t>;

// ── pipe_channel: bootstraps a connected pair ──────────────────────────────────

struct pipe_channel {
  std::shared_ptr<pipe_end_state> client_st;
  std::shared_ptr<pipe_end_state> server_st;
  std::atomic<bool> closed{false};

  static std::shared_ptr<pipe_channel> create() {
    const std::string path = make_anon_pipe_path();
    auto ch = std::make_shared<pipe_channel>();

    auto sst = std::make_shared<pipe_end_state>();
    auto cst = std::make_shared<pipe_end_state>();

    uv_loop_init(&sst->loop);
    uv_pipe_init(&sst->loop, &sst->handle, 0);

    uv_loop_init(&cst->loop);
    uv_pipe_init(&cst->loop, &cst->handle, 0);

    // ── Bootstrap: bind, listen, connect ──────────────────────────────────────
    uv_pipe_t listener{};
    uv_pipe_init(&sst->loop, &listener, 0);

    int r = uv_pipe_bind(&listener, path.c_str());
    if (r != 0) {
      throw std::runtime_error(std::string{"pipe_channel: uv_pipe_bind: "} + uv_strerror(r));
    }

    struct bootstrap_ctx {
      pipe_end_state* sst;
      uv_pipe_t* listener;
      bool accepted{false};
      bool connected{false};
    };
    bootstrap_ctx bs{sst.get(), &listener};
    listener.data = &bs;

    r = uv_listen(reinterpret_cast<uv_stream_t*>(&listener), 1, [](uv_stream_t* s, int status) {
      if (status < 0)
        return;
      auto* ctx = static_cast<bootstrap_ctx*>(s->data);
      uv_accept(s, reinterpret_cast<uv_stream_t*>(&ctx->sst->handle));
      ctx->accepted = true;
    });
    if (r != 0)
      throw std::runtime_error("pipe_channel: uv_listen failed");

    uv_connect_t connect_req{};
    cst->handle.data = &bs; // temporarily, reset below
    uv_pipe_connect(&connect_req, &cst->handle, path.c_str(), [](uv_connect_t* req, int /*status*/) {
      auto* ctx = static_cast<bootstrap_ctx*>(req->handle->data);
      ctx->connected = true;
    });

    // Run both loops until both endpoints are connected.
    while (!bs.accepted || !bs.connected) {
      uv_run(&sst->loop, UV_RUN_NOWAIT);
      uv_run(&cst->loop, UV_RUN_NOWAIT);
    }

    // Close the listener synchronously before starting background threads.
    // listener is a stack variable; it must be fully closed here so the
    // loop thread never touches it after create() returns.
    bool listener_closed = false;
    listener.data = &listener_closed;
    uv_close(reinterpret_cast<uv_handle_t*>(&listener), [](uv_handle_t* h) { *static_cast<bool*>(h->data) = true; });
    while (!listener_closed)
      uv_run(&sst->loop, UV_RUN_NOWAIT);

    // ── Set up async handles and start background threads ─────────────────────
    sst->init_asyncs();
    sst->start_loop();

    cst->init_asyncs();
    cst->start_loop();

    ch->server_st = std::move(sst);
    ch->client_st = std::move(cst);
    return ch;
  }
};

// ── pipe_client_transport ─────────────────────────────────────────────────────

pipe_client_transport::pipe_client_transport(std::shared_ptr<pipe_channel> ch) : ch_(std::move(ch)) {}

void pipe_client_transport::open(bison::dynamic /*params*/) {}

void pipe_client_transport::send(bison::buffer frame) {
  if (!ch_ || ch_->closed.load())
    throw std::runtime_error("pipe_client_transport::send: closed");
  ch_->client_st->enqueue_frame(frame);
}

bool pipe_client_transport::receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
  if (!ch_)
    return false;
  return ch_->client_st->dequeue_frame(frame, timeout);
}

void pipe_client_transport::shutdown() {
  if (!ch_)
    return;
  ch_->closed.store(true);
  ch_->client_st->stop();
}

// ── pipe_server_connection ────────────────────────────────────────────────────

pipe_server_connection::pipe_server_connection(std::shared_ptr<pipe_channel> ch) : ch_(std::move(ch)) {}

void pipe_server_connection::send(bison::buffer frame) {
  if (!ch_ || ch_->closed.load())
    throw std::runtime_error("pipe_server_connection::send: closed");
  ch_->server_st->enqueue_frame(frame);
}

bool pipe_server_connection::receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
  if (!ch_)
    return false;
  return ch_->server_st->dequeue_frame(frame, timeout);
}

void pipe_server_connection::close() {
  if (!ch_)
    return;
  ch_->closed.store(true);
  ch_->server_st->stop();
}

bool pipe_server_connection::is_closed() const {
  return !ch_ || ch_->closed.load();
}

// ── pipe_server_transport ─────────────────────────────────────────────────────

void pipe_server_transport::start(bison::dynamic /*params*/) {
  stopped_.store(false);
}

pipe_client_transport pipe_server_transport::connect() {
  auto ch = pipe_channel::create();
  pending_.withWLock([&](auto& q) { q.push(ch); });
  pending_.notify_one();
  return pipe_client_transport{ch};
}

std::unique_ptr<server_connection_iface> pipe_server_transport::accept(std::chrono::milliseconds timeout) {
  if (!pending_.wait_for(timeout, [this](auto& q) { return !q.empty() || stopped_.load(); }))
    return nullptr;
  return pending_.withWLock([](auto& q) -> std::unique_ptr<server_connection_iface> {
    if (q.empty())
      return nullptr;
    auto ch = std::move(q.front());
    q.pop();
    return std::make_unique<pipe_server_connection>(std::move(ch));
  });
}

void pipe_server_transport::stop() {
  stopped_.store(true);
  pending_.notify_all();
}

} // namespace bdg::bison::rmi::transport
