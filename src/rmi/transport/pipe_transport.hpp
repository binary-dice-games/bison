// MIT License © 2025 Binary Dice Games
/**
 * @file pipe_transport.hpp
 * @brief Anonymous-pipe transport for in-process and inter-process RMI.
 *
 * Uses two unidirectional OS pipes per channel (one client→server, one
 * server→client) to form a full-duplex connection.  Frames are delimited
 * with the same 4-byte big-endian length prefix used by stream_transport.
 *
 * Platform support:
 *  - Linux  — `pipe2(O_CLOEXEC)` + `poll()` for timeout-aware receive.
 *  - Windows — `CreatePipe` + `PeekNamedPipe` for timeout-aware receive.
 *
 * @note `pipe_channel` is an opaque type whose definition lives in the .cpp.
 *       Callers only interact with the transport classes below.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <queue>

namespace bdg::bison::rmi::transport {

/** @brief Platform-specific pipe handles and closed flag. Defined in .cpp. */
struct pipe_channel;

// ── Client-side transport ─────────────────────────────────────────────────────

/**
 * @brief Client endpoint backed by a pair of anonymous OS pipes.
 *
 * Obtained by calling `pipe_server_transport::connect()`.  `open()` is a
 * no-op; the pipes are created and ready at construction time.
 */
class pipe_client_transport : public client_transport_iface {
 public:
  /** @brief Construct from a shared pipe channel. */
  explicit pipe_client_transport(std::shared_ptr<pipe_channel> ch);

  /** @brief No-op; pipes are ready at construction. */
  void open(bison::dynamic params) override;

  /**
   * @brief Write one length-prefixed frame to the server side.
   * @throws std::runtime_error on write failure.
   */
  void send(bison::buffer frame) override;

  /**
   * @brief Read one length-prefixed frame from the server side.
   * @param frame   Output frame buffer.
   * @param timeout Maximum wait before returning `false`.
   * @return `true` on success; `false` on timeout, EOF, or shutdown.
   */
  bool receive(bison::buffer& frame, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Signal shutdown; pending and future receives return `false`. */
  void shutdown() override;

 private:
  std::shared_ptr<pipe_channel> ch_;
};

// ── Server-side connection ────────────────────────────────────────────────────

/**
 * @brief Server connection backed by a pair of anonymous OS pipes.
 *
 * Produced by `pipe_server_transport::accept()`.
 */
class pipe_server_connection : public server_connection_iface {
 public:
  /** @brief Construct from a shared pipe channel. */
  explicit pipe_server_connection(std::shared_ptr<pipe_channel> ch);

  /**
   * @brief Write one length-prefixed frame to the client side.
   * @throws std::runtime_error on write failure.
   */
  void send(bison::buffer frame) override;

  /**
   * @brief Read one length-prefixed frame from the client side.
   * @param frame   Output frame buffer.
   * @param timeout Maximum wait before returning `false`.
   * @return `true` on success; `false` on timeout, EOF, or close.
   */
  bool receive(bison::buffer& frame, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Mark the connection closed; pending and future receives return `false`. */
  void close() override;

  /** @brief Return whether `close()` has been called. */
  bool is_closed() const override;

 private:
  std::shared_ptr<pipe_channel> ch_;
};

// ── Server-side transport ─────────────────────────────────────────────────────

/**
 * @brief Pipe-based server transport with an in-process `connect()` factory.
 *
 * Mirrors `memory_server_transport`: call `connect()` to create a client
 * endpoint and enqueue the matching server end for `accept()`.  This makes
 * the transport suitable for same-process unit tests as well as cross-thread
 * use where the handles are passed to child threads.
 */
class pipe_server_transport : public server_transport_iface {
 public:
  pipe_server_transport() = default;
  pipe_server_transport(const pipe_server_transport&) = delete;
  pipe_server_transport& operator=(const pipe_server_transport&) = delete;

  /** @brief Ready the transport to accept connections. */
  void start(bison::dynamic params) override;

  /**
   * @brief Create a client endpoint and enqueue the matching server connection.
   * @return A `pipe_client_transport` wired to the next `accept()` result.
   */
  pipe_client_transport connect();

  /**
   * @brief Accept the next pending connection.
   * @param timeout Maximum wait duration.
   * @return Server connection on success; `nullptr` on timeout or stop.
   */
  std::unique_ptr<server_connection_iface> accept(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Stop the transport; pending and future accepts return `nullptr`. */
  void stop() override;

 private:
  bison::synchronized<std::queue<std::shared_ptr<pipe_channel>>> pending_;
  std::atomic<bool> stopped_{false};
};

} // namespace bdg::bison::rmi::transport
