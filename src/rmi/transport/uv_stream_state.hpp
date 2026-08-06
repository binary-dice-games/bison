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
 *
 * @par Shared free functions
 * `uv_close_if_active()`, `on_uv_write_done()`, and `uv_flush_write_queue()`
 * factor out the write-queue-drain-and-uv_write pump and close-handle
 * pattern used by `uv_stream_state::on_send`/`on_stop` above. They take
 * plain libuv handles/queues rather than a `uv_stream_state`, so
 * `term_transport.cpp`'s writer (which has an identically-shaped send
 * queue but a separate, non-bidirectional handle and doesn't otherwise fit
 * this template) reuses them too.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/transport/frame_parser.hpp"

#include <uv.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <queue>
#include <thread>
#include <vector>

// BISON_NATIVE_WINDOWS (not _WIN32) is the right guard here: MSYS2 still
// defines _WIN32 (it's a mingw-toolchain build) but shares Linux's POSIX
// signal semantics -- including a real, deliverable SIGPIPE -- via its
// runtime, same as every other POSIX-vs-native-Windows split in this
// codebase (see CLAUDE.md's platform-support rules). BISON_NATIVE_WINDOWS is
// only defined by CMakeLists.txt's `WIN32 AND NOT MSYS AND NOT CYGWIN`
// branch, so this correctly stays active for both Linux and MSYS2.
#if !defined(BISON_NATIVE_WINDOWS)
#include <csignal>
#endif

namespace bdg::bison::rmi::transport {

#if !defined(BISON_NATIVE_WINDOWS)
namespace {

/**
 * @brief Ignore SIGPIPE once per process on Linux/MSYS2.
 *
 * Writing to a socket whose peer has already reset/closed the connection
 * raises SIGPIPE, which defaults to terminating the process instead of the
 * write simply failing with EPIPE -- a completely normal occurrence for any
 * network server (the peer disconnecting mid-write) that a caller should be
 * able to handle via a return code, not a signal. libuv only guards against
 * this via `SO_NOSIGPIPE` where the platform provides it (BSD/macOS); Linux
 * (and MSYS2, which shares its POSIX signal semantics) has neither that
 * socket option nor `MSG_NOSIGNAL` wired into libuv's own write path, so
 * every libuv-backed transport (socket, named pipe, TLS) is otherwise
 * exposed. All of them construct a `uv_stream_state`, making this the
 * natural single place to harden the whole transport layer.
 */
struct ignore_sigpipe_once {
  ignore_sigpipe_once() {
    std::signal(SIGPIPE, SIG_IGN);
  }
};
const ignore_sigpipe_once ignore_sigpipe_once_instance;

} // namespace
#endif

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

/** @brief Close @p h unless it is already closing. Shared by every on_stop callback below. */
inline void uv_close_if_active(uv_handle_t* h) {
  if (!uv_is_closing(h))
    uv_close(h, nullptr);
}

/** @brief `uv_write()` completion callback pairing with `uv_write_req`. */
inline void on_uv_write_done(uv_write_t* req, int /*status*/) {
  delete reinterpret_cast<uv_write_req*>(req);
}

/**
 * @brief Drain @p queue, issuing one `uv_write()` per entry against @p target.
 *
 * Shared write-queue pump used by `uv_stream_state::on_send` and by
 * `term_transport.cpp`'s writer half, which has an identically-shaped send
 * queue but a separate (non-bidirectional) handle.
 */
inline void uv_flush_write_queue(bison::synchronized<std::queue<std::vector<uint8_t>>>& queue, uv_stream_t* target) {
  std::queue<std::vector<uint8_t>> pending;
  queue.withWLock([&](auto& q) { std::swap(pending, q); });
  while (!pending.empty()) {
    auto* wr = new uv_write_req;
    wr->data = std::move(pending.front());
    pending.pop();
    uv_buf_t buf = uv_buf_init(reinterpret_cast<char*>(wr->data.data()), static_cast<unsigned>(wr->data.size()));
    uv_write(&wr->req, target, &buf, 1, on_uv_write_done);
  }
}

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
  frame_parser parser;
  std::vector<uint8_t> read_buf = std::vector<uint8_t>(65536);

  // ── Receive queue: loop thread → caller ────────────────────────────────────
  bison::synchronized<std::queue<bison::buffer>> recv_queue;
  std::atomic<bool> recv_closed{false};

  // ── Recycled frame buffer: caller thread → loop thread ─────────────────────
  // Populated by dequeue_frame() with a just-superseded frame buffer's spare
  // capacity; drained by the loop thread (on_read()/tls_stream_state's
  // pump_decrypt()) via frame_parser::offer_reuse() so the next frame's
  // allocation can reuse it instead of a fresh malloc.
  bison::synchronized<bison::buffer> recycle_slot;

  // ── Send queue: caller → loop thread ───────────────────────────────────────
  bison::synchronized<std::queue<std::vector<uint8_t>>> send_queue;

  // ── Thread state ───────────────────────────────────────────────────────────
  std::atomic<bool> stopped{false};
  std::thread loop_thread;
  bool asyncs_ready{false};

  uv_stream_state() = default;
  ~uv_stream_state() {
    stop();
  }

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
    asyncs_ready = true;
  }

  /** @brief Launch the background I/O thread. Call after `init_asyncs()`. */
  void start_loop() {
    loop_thread = std::thread([this] {
      uv_read_start(reinterpret_cast<uv_stream_t*>(&handle), alloc_cb, on_read);
      uv_run(&loop, UV_RUN_DEFAULT);
      recv_closed.store(true);
      recv_queue.notify_all();
      uv_loop_close(&loop);
    });
  }

  /**
   * @brief Signal the loop to stop and join the background thread.
   *
   * If `init_asyncs()` was never called (e.g. an open/connect attempt threw
   * before reaching it), `send_async`/`stop_async` were never passed to
   * `uv_async_init()`, so signalling them is undefined behavior. In that
   * case `start_loop()` was also never called, so no background thread is
   * running the loop; it is safe to close the handle and drain the loop
   * synchronously on the calling thread instead.
   *
   * If `stop()` is instead invoked synchronously from a callback running on
   * `loop_thread` itself (e.g. a disconnect/error handler reached through
   * `on_read()`), `loop_thread.join()` would be a thread joining itself --
   * `std::thread::join()` throws `std::system_error("Resource deadlock
   * avoided")` in that case. `uv_async_send()` still safely schedules
   * `on_stop` to run and close every handle on the next loop iteration
   * (from any thread, including the loop thread), so `uv_run()` returns and
   * the thread finishes on its own; detach instead of joining when that
   * self-call is detected.
   */
  void stop() {
    if (stopped.exchange(true))
      return;
    if (!asyncs_ready) {
      uv_close_if_active(reinterpret_cast<uv_handle_t*>(&handle));
      uv_run(&loop, UV_RUN_DEFAULT);
      uv_loop_close(&loop);
      return;
    }
    if (!loop_thread.joinable())
      return;
    uv_async_send(&stop_async);
    if (std::this_thread::get_id() == loop_thread.get_id()) {
      loop_thread.detach();
      return;
    }
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
    recv_queue.wait_for(timeout, [&](auto& q) {
      if (q.empty() && !recv_closed.load())
        return false;
      if (!q.empty()) {
        if (frame.capacity() > 0)
          recycle_slot.withWLock([&](auto& spare) {
            if (frame.capacity() > spare.capacity())
              spare = std::move(frame);
          });
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
      st->recv_queue.notify_all();
      uv_read_stop(stream);
      return;
    }
    if (nread == 0)
      return;

    bison::buffer recycled;
    st->recycle_slot.withWLock([&](auto& spare) { recycled = std::move(spare); });
    if (recycled.capacity() > 0)
      st->parser.offer_reuse(std::move(recycled));

    const bool ok = st->parser.feed(st->read_buf.data(), static_cast<size_t>(nread), [st](bison::buffer&& frame) {
      st->recv_queue.withWLock([&](auto& q) { q.push(std::move(frame)); });
      st->recv_queue.notify_one();
    });
    if (!ok) {
      // Declared frame length exceeded frame_parser::kMaxFrameBytes -- treat
      // as a fatal protocol error, same as a peer-initiated close.
      st->recv_closed.store(true);
      st->recv_queue.notify_all();
      uv_read_stop(stream);
    }
  }

  static void on_send(uv_async_t* async) {
    auto* st = static_cast<uv_stream_state*>(async->data);
    uv_flush_write_queue(st->send_queue, reinterpret_cast<uv_stream_t*>(&st->handle));
  }

  static void on_stop(uv_async_t* async) {
    auto* st = static_cast<uv_stream_state*>(async->data);
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(&st->handle));
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(&st->send_async));
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(&st->stop_async));
  }
};

} // namespace bdg::bison::rmi::transport
