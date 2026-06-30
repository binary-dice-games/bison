// MIT License © 2025 Binary Dice Games
/**
 * @file uv_stream_state.hpp
 * @brief Shared per-connection libuv I/O state for uv_stream_t-based transports.
 *
 * All three libuv-backed transports (pipe_transport, named_pipe_transport,
 * socket_transport) require an identical per-connection bundle: a uv_loop_t,
 * background I/O thread, incremental frame parser, send/receive queues, and
 * five libuv callbacks.  This template captures that common state so each
 * transport only needs to supply the concrete handle type (uv_tcp_t or
 * uv_pipe_t) and the one handle-specific init call.
 *
 * @par Usage pattern
 *  1. Default-construct.
 *  2. Call `uv_loop_init(&st->loop)` + the handle-specific init function
 *     (`uv_tcp_init` or `uv_pipe_init`) to bring the handle up.
 *  3. Call `st->init_asyncs()` to wire up `send_async`, `stop_async`, and
 *     `handle.data`.
 *  4. Call `st->start_loop()` to launch the background I/O thread.
 *  5. Use `st->enqueue_frame()` / `st->dequeue_frame()` from other threads.
 *  6. Call `st->stop()` (or let the destructor do it) to shut down.
 */
#pragma once

#include "src/bison/bison.hpp"

#include <uv.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <queue>
#include <thread>
#include <vector>

namespace bdg::bison::rmi::transport {

// ── write_req ─────────────────────────────────────────────────────────────────

/**
 * @brief Heap-allocated write request; one per uv_write() call.
 *
 * Lifetime is managed by the loop thread: allocated in on_send, freed in
 * on_write_done.
 */
struct uv_write_req {
  uv_write_t req{};
  std::vector<uint8_t> data;
};

// ── uv_stream_state ───────────────────────────────────────────────────────────

/**
 * @brief Per-connection libuv event loop, frame queues, and shared callbacks.
 *
 * @tparam Handle  libuv stream handle type: `uv_tcp_t` or `uv_pipe_t`.
 */
template <typename Handle>
struct uv_stream_state {
  // ── libuv handles ──────────────────────────────────────────────────────────
  uv_loop_t loop{};
  Handle handle{};
  uv_async_t send_async{};
  uv_async_t stop_async{};

  // ── Incremental frame parser (loop thread only; no locking needed) ─────────
  uint8_t hdr[4]{};
  uint32_t hdr_pos{0};
  uint32_t payload_left{0};
  bison::buffer partial;
  std::vector<uint8_t> read_buf = std::vector<uint8_t>(65536);

  // ── Receive queue: loop thread → caller ────────────────────────────────────
  bison::synchronized<std::queue<bison::buffer>> recv_queue;
  std::condition_variable_any recv_cv;
  std::atomic<bool> recv_closed{false};

  // ── Send queue: caller → loop thread ───────────────────────────────────────
  bison::synchronized<std::queue<std::vector<uint8_t>>> send_queue;

  // ── Thread state ───────────────────────────────────────────────────────────
  std::atomic<bool> stopped{false};
  std::thread loop_thread;

  uv_stream_state() = default;
  ~uv_stream_state() { stop(); }

  uv_stream_state(const uv_stream_state&) = delete;
  uv_stream_state& operator=(const uv_stream_state&) = delete;
  uv_stream_state(uv_stream_state&&) = delete;
  uv_stream_state& operator=(uv_stream_state&&) = delete;

  // ── Lifecycle ──────────────────────────────────────────────────────────────

  /**
   * @brief Wire up `send_async`, `stop_async`, and `handle.data`.
   *
   * Call after `uv_loop_init(&loop)` and the handle-specific init function.
   */
  void init_asyncs() {
    handle.data = this;
    uv_async_init(&loop, &send_async, on_send);
    send_async.data = this;
    uv_async_init(&loop, &stop_async, on_stop);
    stop_async.data = this;
  }

  /** @brief Launch the background I/O thread. Call after `init_asyncs()`. */
  void start_loop() {
    loop_thread = std::thread([this] {
      uv_read_start(reinterpret_cast<uv_stream_t*>(&handle), alloc_cb, on_read);
      uv_run(&loop, UV_RUN_DEFAULT);
      recv_closed.store(true);
      recv_cv.notify_all();
      uv_loop_close(&loop);
    });
  }

  /** @brief Signal the loop to stop and join the background thread. */
  void stop() {
    if (stopped.exchange(true))
      return;
    uv_async_send(&stop_async);
    if (loop_thread.joinable())
      loop_thread.join();
  }

  // ── Frame I/O ──────────────────────────────────────────────────────────────

  /**
   * @brief Enqueue @p frame for async send to the peer.
   *
   * Prepends a 4-byte big-endian length prefix and signals the loop thread.
   */
  void enqueue_frame(const bison::buffer& frame) {
    std::vector<uint8_t> data(4 + frame.size());
    const uint32_t net_len = byte_swap(static_cast<uint32_t>(frame.size()));
    std::memcpy(data.data(), &net_len, 4);
    if (!frame.empty())
      std::memcpy(data.data() + 4, frame.data(), frame.size());
    send_queue.withWLock([&](auto& q) { q.push(std::move(data)); });
    uv_async_send(&send_async);
  }

  /**
   * @brief Block until a complete frame is available or the timeout elapses.
   * @param frame   Populated on success.
   * @param timeout Maximum wait duration.
   * @return `true` if a frame was placed in @p frame; `false` on timeout or close.
   */
  bool dequeue_frame(bison::buffer& frame, std::chrono::milliseconds timeout) {
    bool got = false;
    recv_queue.wait_for(recv_cv, timeout, [&](auto& q) {
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

  // ── Static libuv callbacks ─────────────────────────────────────────────────

  static void alloc_cb(uv_handle_t* h, size_t /*suggested*/, uv_buf_t* buf) {
    auto* st = static_cast<uv_stream_state*>(h->data);
    buf->base = reinterpret_cast<char*>(st->read_buf.data());
    buf->len = static_cast<decltype(buf->len)>(st->read_buf.size());
  }

  static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* /*buf*/) {
    auto* st = static_cast<uv_stream_state*>(stream->data);
    if (nread < 0) {
      st->recv_closed.store(true);
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
          uint32_t net_hdr{};
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
          st->recv_queue.withWLock([&](auto& q) { q.push(std::move(st->partial)); });
          st->recv_cv.notify_one();
          st->partial = bison::buffer{};
          st->hdr_pos = 0;
        }
      }
    }
  }

  static void on_write_done(uv_write_t* req, int /*status*/) {
    delete reinterpret_cast<uv_write_req*>(req);
  }

  static void on_send(uv_async_t* async) {
    auto* st = static_cast<uv_stream_state*>(async->data);
    std::queue<std::vector<uint8_t>> q;
    st->send_queue.withWLock([&](auto& sq) { std::swap(q, sq); });
    while (!q.empty()) {
      auto* wr = new uv_write_req;
      wr->data = std::move(q.front());
      q.pop();
      uv_buf_t b = uv_buf_init(reinterpret_cast<char*>(wr->data.data()),
                                static_cast<unsigned>(wr->data.size()));
      uv_write(&wr->req, reinterpret_cast<uv_stream_t*>(&st->handle), &b, 1, on_write_done);
    }
  }

  static void on_stop(uv_async_t* async) {
    auto* st = static_cast<uv_stream_state*>(async->data);
    const auto close_if_active = [](uv_handle_t* h) {
      if (!uv_is_closing(h))
        uv_close(h, nullptr);
    };
    close_if_active(reinterpret_cast<uv_handle_t*>(&st->handle));
    close_if_active(reinterpret_cast<uv_handle_t*>(&st->send_async));
    close_if_active(reinterpret_cast<uv_handle_t*>(&st->stop_async));
  }
};

} // namespace bdg::bison::rmi::transport
