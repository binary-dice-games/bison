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

// ── Per-connection I/O state ───────────────────────────────────────────────────

struct pipe_conn_state {
  uv_loop_t loop{};
  uv_pipe_t handle{};
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
    auto* st = static_cast<pipe_conn_state*>(h->data);
    buf->base = reinterpret_cast<char*>(st->read_buf.data());
    buf->len = static_cast<decltype(buf->len)>(st->read_buf.size());
  }

  static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t*) {
    auto* st = static_cast<pipe_conn_state*>(stream->data);
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

    const auto* p = st->read_buf.data();
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
    auto* st = static_cast<pipe_conn_state*>(async->data);
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
    auto* st = static_cast<pipe_conn_state*>(async->data);
    const auto close_if_active = [](uv_handle_t* h) {
      if (!uv_is_closing(h))
        uv_close(h, nullptr);
    };
    close_if_active(reinterpret_cast<uv_handle_t*>(&st->handle));
    close_if_active(reinterpret_cast<uv_handle_t*>(&st->send_async));
    close_if_active(reinterpret_cast<uv_handle_t*>(&st->stop_async));
  }

  // ── Lifecycle ─────────────────────────────────────────────────────────────

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

  ~pipe_conn_state() {
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

  std::mutex accept_mtx;
  std::condition_variable accept_cv;
  std::queue<std::unique_ptr<named_pipe_server_connection>> accept_queue;
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

  cs->handle.data = cs.get();
  uv_async_init(&cs->loop, &cs->send_async, pipe_conn_state::on_send);
  cs->send_async.data = cs.get();
  uv_async_init(&cs->loop, &cs->stop_async, pipe_conn_state::on_stop);
  cs->stop_async.data = cs.get();
  cs->start_loop();

  auto conn = std::make_unique<named_pipe_conn>();
  conn->cs = std::move(cs);
  auto wrapped = make_server_connection(std::move(conn));
  {
    std::lock_guard<std::mutex> lk(ss->accept_mtx);
    ss->accept_queue.push(std::move(wrapped));
  }
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

  cs->handle.data = cs.get();
  uv_async_init(&cs->loop, &cs->send_async, pipe_conn_state::on_send);
  cs->send_async.data = cs.get();
  uv_async_init(&cs->loop, &cs->stop_async, pipe_conn_state::on_stop);
  cs->stop_async.data = cs.get();
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

std::unique_ptr<server_connection_iface> named_pipe_server_transport::accept(std::chrono::milliseconds timeout) {
  if (!state_)
    return nullptr;
  std::unique_lock<std::mutex> lk(state_->accept_mtx);
  if (!state_->accept_cv.wait_for(lk, timeout, [this] { return !state_->accept_queue.empty() || stopped_.load(); }))
    return nullptr;
  if (state_->accept_queue.empty())
    return nullptr;
  auto conn = std::move(state_->accept_queue.front());
  state_->accept_queue.pop();
  return conn;
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
