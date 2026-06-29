// MIT License © 2025 Binary Dice Games
/**
 * @file pipe_transport_uv.cpp
 * @brief Anonymous-pipe transport reimplemented using libuv.
 *
 * Establishes a full-duplex channel between in-process endpoints using a
 * pair of connected uv_pipe_t handles.  Both ends own a uv_loop_t that runs
 * on a dedicated background thread and bridges incoming frames into a
 * mutex-protected queue for synchronous receive() calls.
 *
 * Framing: 4-byte big-endian length prefix followed by payload, identical
 * to socket_transport_uv and named_pipe_transport_uv.
 */
#include "src/rmi/transport/pipe_transport.hpp"

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

// ── Unique path per channel ────────────────────────────────────────────────────

static std::string make_anon_pipe_path() {
  static std::atomic<uint64_t> counter{0};
  const auto id = counter.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
  return std::string{"\\\\.\\pipe\\bison-anon-"} + std::to_string(id);
#else
  return std::string{"/tmp/.bison-anon-"} + std::to_string(id) + ".sock";
#endif
}

// ── Per-connection state ───────────────────────────────────────────────────────

struct pipe_end_state {
  uv_loop_t loop{};
  uv_pipe_t handle{};
  uv_async_t send_async{};
  uv_async_t stop_async{};

  // Incremental frame parser — owned by the loop thread.
  uint8_t hdr[4]{};
  uint32_t hdr_pos{0};
  uint32_t payload_left{0};
  bison::buffer partial;
  std::vector<uint8_t> read_buf = std::vector<uint8_t>(65536);

  // Receive queue: loop thread → caller thread.
  std::mutex recv_mtx;
  std::condition_variable recv_cv;
  std::queue<bison::buffer> recv_queue;
  std::atomic<bool> recv_closed{false};

  // Send queue: caller thread → loop thread.
  std::mutex send_mtx;
  std::queue<std::vector<uint8_t>> send_queue;

  std::atomic<bool> stopped{false};
  std::thread loop_thread;

  // ── libuv callbacks (static, accessed via handle->data == this) ───────────

  static void alloc_cb(uv_handle_t* h, size_t /*sug*/, uv_buf_t* buf) {
    auto* st = static_cast<pipe_end_state*>(h->data);
    buf->base = reinterpret_cast<char*>(st->read_buf.data());
    buf->len = static_cast<decltype(buf->len)>(st->read_buf.size());
  }

  static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* st = static_cast<pipe_end_state*>(stream->data);
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
    auto* st = static_cast<pipe_end_state*>(async->data);
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
    auto* st = static_cast<pipe_end_state*>(async->data);
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

  ~pipe_end_state() {
    stop();
  }

  // ── Public send / receive ─────────────────────────────────────────────────

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
    sst->handle.data = sst.get();
    uv_async_init(&sst->loop, &sst->send_async, pipe_end_state::on_send);
    sst->send_async.data = sst.get();
    uv_async_init(&sst->loop, &sst->stop_async, pipe_end_state::on_stop);
    sst->stop_async.data = sst.get();
    sst->start_loop();

    cst->handle.data = cst.get();
    uv_async_init(&cst->loop, &cst->send_async, pipe_end_state::on_send);
    cst->send_async.data = cst.get();
    uv_async_init(&cst->loop, &cst->stop_async, pipe_end_state::on_stop);
    cst->stop_async.data = cst.get();
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
  {
    std::lock_guard<std::mutex> lk(mtx_);
    pending_.push(ch);
  }
  cv_.notify_one();
  return pipe_client_transport{ch};
}

std::unique_ptr<server_connection_iface> pipe_server_transport::accept(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lk(mtx_);
  if (!cv_.wait_for(lk, timeout, [this] { return !pending_.empty() || stopped_.load(); }))
    return nullptr;
  if (pending_.empty())
    return nullptr;
  auto ch = std::move(pending_.front());
  pending_.pop();
  return std::make_unique<pipe_server_connection>(std::move(ch));
}

void pipe_server_transport::stop() {
  stopped_.store(true);
  cv_.notify_all();
}

} // namespace bdg::bison::rmi::transport
