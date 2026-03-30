// MIT License © 2025 Binary Dice Games
/**
 * @file memory_transport.hpp
 * @brief In-process transport implementation for RMI testing and embedding.
 */
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

/**
 * @brief Shared bidirectional queues and synchronization primitives.
 *
 * Represents one logical duplex connection between one client endpoint and
 * one server endpoint.
 */
struct memory_channel {
  std::mutex mtx;
  std::condition_variable cv_c2s;
  std::condition_variable cv_s2c;
  std::queue<bison::buffer> c2s_queue;
  std::queue<bison::buffer> s2c_queue;
  std::atomic<bool> closed{false};
};

/**
 * @brief Client-side endpoint of the in-memory transport.
 */
class memory_client_transport {
 public:
  /** @brief Construct from a shared channel state object. */
  explicit memory_client_transport(std::shared_ptr<memory_channel> ch);

  /** @brief Open the endpoint. Parameters are accepted for API compatibility.
   */
  void open(bison::dynamic&& params);

  /** @brief Send one encoded frame to the server side. */
  void send(bison::buffer frame);

  /**
   * @brief Receive one frame from the server side.
   * @param frame Output frame buffer.
   * @param timeout Maximum wait time before returning `false`.
   * @return `true` when a frame was received; otherwise `false`.
   */
  bool receive(
      bison::buffer& frame,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

  /** @brief Close the endpoint and wake blocked waiters. */
  void shutdown();

 private:
  std::shared_ptr<memory_channel> ch_;
};

/**
 * @brief Server-side connection object for a single accepted channel.
 */
class memory_server_connection {
 public:
  /** @brief Construct from a shared channel state object. */
  explicit memory_server_connection(std::shared_ptr<memory_channel> ch);

  /** @brief Send one encoded frame to the client side. */
  void send(bison::buffer frame);

  /**
   * @brief Receive one frame from the client side.
   * @param frame Output frame buffer.
   * @param timeout Maximum wait time before returning `false`.
   * @return `true` when a frame was received; otherwise `false`.
   */
  bool receive(
      bison::buffer& frame,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

  /** @brief Mark the connection as closed and notify waiters. */
  void close();

  /** @brief Return whether this connection has been closed. */
  bool is_closed() const;

 private:
  std::shared_ptr<memory_channel> ch_;
};

/**
 * @brief Server-side listener and accept queue for in-memory channels.
 */
class memory_server_transport {
 public:
  memory_server_transport() = default;
  memory_server_transport(const memory_server_transport&) = delete;
  memory_server_transport& operator=(const memory_server_transport&) = delete;

  /** @brief Start listening for in-process connection requests. */
  void start(bison::dynamic params);

  /** @brief Create a client endpoint connected to this server transport. */
  memory_client_transport connect();

  /**
   * @brief Accept the next pending client connection.
   * @param timeout Maximum wait duration for a pending connection.
   * @return Server-side connection on success, otherwise `std::nullopt`.
   */
  std::optional<memory_server_connection> accept(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

  /** @brief Stop listening and wake blocked accept calls. */
  void stop();

 private:
  std::mutex mtx_;
  std::condition_variable cv_;
  std::queue<std::shared_ptr<memory_channel>> pending_;
  std::atomic<bool> stopped_{false};
};

} // namespace bdg::bison::rmi::transport
