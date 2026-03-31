// MIT License © 2025 Binary Dice Games
/**
 * @file memory_transport.cpp
 * @brief In-process transport implementation used by the RMI runtime.
 */
#include "src/rmi/transport/memory_transport.hpp"

namespace bdg::bison::rmi::transport {

// memory_client_transport
/** @copydoc
 * bdg::bison::rmi::transport::memory_client_transport::memory_client_transport
 */
memory_client_transport::memory_client_transport(
    std::shared_ptr<memory_channel> ch)
    : ch_(std::move(ch)) {}

/** @copydoc bdg::bison::rmi::transport::memory_client_transport::open */
void memory_client_transport::open(bison::dynamic&& /*params*/) {}

/** @copydoc bdg::bison::rmi::transport::memory_client_transport::send */
void memory_client_transport::send(bison::buffer frame) {
  {
    std::lock_guard<std::mutex> lk(ch_->mtx);
    ch_->c2s_queue.push(std::move(frame));
  }
  ch_->cv_c2s.notify_one();
}

/** @copydoc bdg::bison::rmi::transport::memory_client_transport::receive */
bool memory_client_transport::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lk(ch_->mtx);
  if (!ch_->cv_s2c.wait_for(lk, timeout, [this] {
        return !ch_->s2c_queue.empty() || ch_->closed.load();
      }))
    return false;
  if (ch_->s2c_queue.empty())
    return false;
  frame = std::move(ch_->s2c_queue.front());
  ch_->s2c_queue.pop();
  return true;
}

/** @copydoc bdg::bison::rmi::transport::memory_client_transport::shutdown */
void memory_client_transport::shutdown() {
  ch_->closed.store(true);
  ch_->cv_c2s.notify_all();
  ch_->cv_s2c.notify_all();
}

// memory_server_connection
/** @copydoc
 * bdg::bison::rmi::transport::memory_server_connection::memory_server_connection
 */
memory_server_connection::memory_server_connection(
    std::shared_ptr<memory_channel> ch)
    : ch_(std::move(ch)) {}

/** @copydoc bdg::bison::rmi::transport::memory_server_connection::send */
void memory_server_connection::send(bison::buffer frame) {
  {
    std::lock_guard<std::mutex> lk(ch_->mtx);
    ch_->s2c_queue.push(std::move(frame));
  }
  ch_->cv_s2c.notify_one();
}

/** @copydoc bdg::bison::rmi::transport::memory_server_connection::receive */
bool memory_server_connection::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lk(ch_->mtx);
  if (!ch_->cv_c2s.wait_for(lk, timeout, [this] {
        return !ch_->c2s_queue.empty() || ch_->closed.load();
      }))
    return false;
  if (ch_->c2s_queue.empty())
    return false;
  frame = std::move(ch_->c2s_queue.front());
  ch_->c2s_queue.pop();
  return true;
}

/** @copydoc bdg::bison::rmi::transport::memory_server_connection::close */
void memory_server_connection::close() {
  ch_->closed.store(true);
  ch_->cv_c2s.notify_all();
  ch_->cv_s2c.notify_all();
}

/** @copydoc bdg::bison::rmi::transport::memory_server_connection::is_closed */
bool memory_server_connection::is_closed() const {
  return ch_->closed.load();
}

// memory_server_transport
/** @copydoc bdg::bison::rmi::transport::memory_server_transport::start */
void memory_server_transport::start(bison::dynamic /*params*/) {
  stopped_.store(false);
}

/** @copydoc bdg::bison::rmi::transport::memory_server_transport::connect */
memory_client_transport memory_server_transport::connect() {
  auto ch = std::make_shared<memory_channel>();
  {
    std::lock_guard<std::mutex> lk(mtx_);
    pending_.push(ch);
  }
  cv_.notify_one();
  return memory_client_transport{std::move(ch)};
}

/** @copydoc bdg::bison::rmi::transport::memory_server_transport::accept */
std::optional<memory_server_connection> memory_server_transport::accept(
    std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lk(mtx_);
  if (!cv_.wait_for(
          lk, timeout, [this] { return !pending_.empty() || stopped_.load(); }))
    return std::nullopt;
  if (pending_.empty())
    return std::nullopt;
  auto ch = std::move(pending_.front());
  pending_.pop();
  return memory_server_connection{std::move(ch)};
}

/** @copydoc bdg::bison::rmi::transport::memory_server_transport::stop */
void memory_server_transport::stop() {
  stopped_.store(true);
  cv_.notify_all();
}

} // namespace bdg::bison::rmi::transport
