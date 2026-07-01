// MIT License © 2025 Binary Dice Games
/**
 * @file memory_transport.hpp
 * @brief In-process transport implementation for RMI testing and embedding.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <queue>

namespace bdg::bison::rmi::transport {

/**
 * @brief Shared bidirectional queues and synchronization primitives.
 *
 * Represents one logical duplex connection between one client endpoint and
 * one server endpoint. Each queue is owned by a `bison::synchronized` wrapper
 * that forces explicit scoped access and provides its own condition variable
 * for pending-item signalling without exposing raw mutexes to callers.
 */
struct memory_channel {
  bison::synchronized<std::queue<bison::buffer>> c2s;
  bison::synchronized<std::queue<bison::buffer>> s2c;
  std::atomic<bool> closed{false};
};

/**
 * @brief Client-side endpoint of the in-memory transport.
 */
class memory_client_transport : public client_transport_iface {
 public:
  /** @brief Construct from a shared channel state object. */
  explicit memory_client_transport(std::shared_ptr<memory_channel> ch);

  /** @brief Open the endpoint. Parameters are accepted for API compatibility.
   */
  void open(bison::dynamic params) override;

  /** @brief Send one encoded frame to the server side. */
  void send(bison::buffer frame) override;

  /**
   * @brief Receive one frame from the server side.
   * @param frame Output frame buffer.
   * @param timeout Maximum wait time before returning `false`.
   * @return `true` when a frame was received; otherwise `false`.
   */
  bool receive(bison::buffer& frame, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Close the endpoint and wake blocked waiters. */
  void shutdown() override;

 private:
  std::shared_ptr<memory_channel> ch_;
};

/**
 * @brief Server-side connection object for a single accepted channel.
 */
class memory_server_connection : public server_connection_iface {
 public:
  /** @brief Construct from a shared channel state object. */
  explicit memory_server_connection(std::shared_ptr<memory_channel> ch);

  /** @brief Send one encoded frame to the client side. */
  void send(bison::buffer frame) override;

  /**
   * @brief Receive one frame from the client side.
   * @param frame Output frame buffer.
   * @param timeout Maximum wait time before returning `false`.
   * @return `true` when a frame was received; otherwise `false`.
   */
  bool receive(bison::buffer& frame, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Mark the connection as closed and notify waiters. */
  void close() override;

  /** @brief Return whether this connection has been closed. */
  bool is_closed() const override;

 private:
  std::shared_ptr<memory_channel> ch_;
};

/**
 * @brief Server-side listener and accept queue for in-memory channels.
 */
class memory_server_transport : public server_transport_iface {
 public:
  memory_server_transport() = default;
  memory_server_transport(const memory_server_transport&) = delete;
  memory_server_transport& operator=(const memory_server_transport&) = delete;

  /** @brief Start listening for in-process connection requests. */
  void start(bison::dynamic params) override;

  /** @brief Create a client endpoint connected to this server transport. */
  memory_client_transport connect();

  /**
   * @brief Accept the next pending client connection.
   * @param timeout Maximum wait duration for a pending connection.
   * @return Server-side connection on success, otherwise `nullptr`.
   */
  std::unique_ptr<server_connection_iface> accept(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Stop listening and wake blocked accept calls. */
  void stop() override;

 private:
  bison::synchronized<std::queue<std::shared_ptr<memory_channel>>> pending_;
  std::atomic<bool> stopped_{false};
};

} // namespace bdg::bison::rmi::transport
