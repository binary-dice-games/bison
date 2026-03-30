// MIT License © 2025 Binary Dice Games
#pragma once

#include "src/core/bison.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>

namespace bdg::bison::rmi::transport {

// ─── Shared bidirectional channel ────────────────────────────────────────────

/**
 * @brief Internal shared state for one client ↔ server connection.
 *
 * Two FIFO queues carry framed byte buffers in each direction; a shared
 * `closed` flag signals orderly shutdown to both ends.
 */
struct memory_channel {
  std::mutex              mtx;
  std::condition_variable cv_c2s;  ///< notified when c2s_queue gains an item
  std::condition_variable cv_s2c;  ///< notified when s2c_queue gains an item
  std::queue<std::vector<char>> c2s_queue;  ///< client → server
  std::queue<std::vector<char>> s2c_queue;  ///< server → client
  std::atomic<bool>       closed{false};
};

// ─── Client-side transport ────────────────────────────────────────────────────

/**
 * @brief Client-side end of an in-memory transport channel.
 *
 * Satisfies the duck-typed client-transport concept expected by `client<T>`:
 *   - `void open(bison::dynamic params)`
 *   - `void send(std::vector<char> frame)`
 *   - `bool receive(std::vector<char>&, std::chrono::milliseconds)`
 *   - `void shutdown()`
 */
class memory_client_transport {
 public:
  explicit memory_client_transport(std::shared_ptr<memory_channel> ch)
      : ch_(std::move(ch)) {}

  void open(bison::dynamic /*params*/) { /* channel already established */ }

  void send(std::vector<char> frame) {
    {
      std::lock_guard<std::mutex> lk(ch_->mtx);
      ch_->c2s_queue.push(std::move(frame));
    }
    ch_->cv_c2s.notify_one();
  }

  bool receive(std::vector<char>& frame,
               std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) {
    std::unique_lock<std::mutex> lk(ch_->mtx);
    if (!ch_->cv_s2c.wait_for(lk, timeout, [this] {
          return !ch_->s2c_queue.empty() || ch_->closed.load();
        })) {
      return false;  // timeout
    }
    if (ch_->s2c_queue.empty()) return false;  // closed with empty queue
    frame = std::move(ch_->s2c_queue.front());
    ch_->s2c_queue.pop();
    return true;
  }

  void shutdown() {
    ch_->closed.store(true);
    ch_->cv_c2s.notify_all();
    ch_->cv_s2c.notify_all();
  }

 private:
  std::shared_ptr<memory_channel> ch_;
};

// ─── Server-side per-client connection ───────────────────────────────────────

/**
 * @brief Server-side end of an in-memory transport channel.
 *
 * Satisfies the duck-typed server-connection concept:
 *   - `void send(std::vector<char> frame)`
 *   - `bool receive(std::vector<char>&, std::chrono::milliseconds)`
 *   - `void close()`
 *   - `bool is_closed() const`
 */
class memory_server_connection {
 public:
  explicit memory_server_connection(std::shared_ptr<memory_channel> ch)
      : ch_(std::move(ch)) {}

  void send(std::vector<char> frame) {
    {
      std::lock_guard<std::mutex> lk(ch_->mtx);
      ch_->s2c_queue.push(std::move(frame));
    }
    ch_->cv_s2c.notify_one();
  }

  bool receive(std::vector<char>& frame,
               std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) {
    std::unique_lock<std::mutex> lk(ch_->mtx);
    if (!ch_->cv_c2s.wait_for(lk, timeout, [this] {
          return !ch_->c2s_queue.empty() || ch_->closed.load();
        })) {
      return false;
    }
    if (ch_->c2s_queue.empty()) return false;
    frame = std::move(ch_->c2s_queue.front());
    ch_->c2s_queue.pop();
    return true;
  }

  void close() {
    ch_->closed.store(true);
    ch_->cv_c2s.notify_all();
    ch_->cv_s2c.notify_all();
  }

  bool is_closed() const { return ch_->closed.load(); }

 private:
  std::shared_ptr<memory_channel> ch_;
};

// ─── Server-side transport ────────────────────────────────────────────────────

/**
 * @brief Server-side in-memory transport.
 *
 * Call `connect()` from the *test* side to obtain a paired
 * `memory_client_transport`.  The server calls `start()` and then `accept()`
 * in its accept loop.
 *
 * Satisfies the duck-typed server-transport concept:
 *   - `void start(bison::dynamic params)`
 *   - `std::optional<memory_server_connection> accept(std::chrono::milliseconds)`
 *   - `void stop()`
 */
class memory_server_transport {
 public:
  void start(bison::dynamic /*params*/) { stopped_.store(false); }

  /**
   * @brief Create a new client transport connected to this server.
   *
   * The corresponding `memory_server_connection` will be returned by the
   * next call to `accept()`.
   */
  memory_client_transport connect() {
    auto ch = std::make_shared<memory_channel>();
    {
      std::lock_guard<std::mutex> lk(mtx_);
      pending_.push(ch);
    }
    cv_.notify_one();
    return memory_client_transport{std::move(ch)};
  }

  std::optional<memory_server_connection> accept(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) {
    std::unique_lock<std::mutex> lk(mtx_);
    if (!cv_.wait_for(lk, timeout, [this] {
          return !pending_.empty() || stopped_.load();
        })) {
      return std::nullopt;
    }
    if (pending_.empty()) return std::nullopt;
    auto ch = std::move(pending_.front());
    pending_.pop();
    return memory_server_connection{std::move(ch)};
  }

  void stop() {
    stopped_.store(true);
    cv_.notify_all();
  }

 private:
  std::mutex                              mtx_;
  std::condition_variable                 cv_;
  std::queue<std::shared_ptr<memory_channel>> pending_;
  std::atomic<bool>                       stopped_{false};
};

} // namespace bdg::bison::rmi::transport
