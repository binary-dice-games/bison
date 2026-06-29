// MIT License © 2025 Binary Dice Games
/**
 * @file named_pipe_transport.hpp
 * @brief Named-pipe / Unix-socket transport for single-host inter-process RMI.
 *
 * Provides a persistent accept loop that accepts multiple sequential or
 * concurrent clients over a named OS channel:
 *
 * - **Windows** — Win32 named pipe (`\\.\pipe\<name>`).
 *   Each `accept()` creates a new pipe instance with
 *   `PIPE_UNLIMITED_INSTANCES` so multiple clients can be served.
 * - **Linux / macOS** — Unix domain socket (`AF_UNIX`, `SOCK_STREAM`).
 *   Semantics are identical to the socket transport but scoped to the
 *   local machine and addressed by a file-system path.
 *
 * Framing: same 4-byte big-endian length prefix as `pipe_transport` and
 * `stream_transport`.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

namespace bdg::bison::rmi::transport {

/** @brief Opaque connection state (platform-specific; defined in .cpp). */
struct named_pipe_conn;

/** @brief Opaque server state (platform-specific; defined in .cpp). */
struct named_pipe_server_state;

// ── Server-side connection ────────────────────────────────────────────────────

/**
 * @brief Server end of a single named-pipe / Unix-socket client connection.
 *
 * Produced by `named_pipe_server_transport::accept()`.
 */
class named_pipe_server_connection : public server_connection_iface {
 public:
  explicit named_pipe_server_connection(std::unique_ptr<named_pipe_conn> conn);
  ~named_pipe_server_connection();

  named_pipe_server_connection(const named_pipe_server_connection&) = delete;
  named_pipe_server_connection& operator=(const named_pipe_server_connection&) = delete;

  /**
   * @brief Write one length-prefixed frame to the client.
   * @throws std::runtime_error on write failure.
   */
  void send(bison::buffer frame) override;

  /**
   * @brief Read one length-prefixed frame from the client.
   * @param frame   Output frame buffer.
   * @param timeout Maximum wait before returning `false`.
   * @return `true` on success; `false` on timeout, EOF, or close.
   */
  bool receive(bison::buffer& frame, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Close the connection; pending receives return `false`. */
  void close() override;

  /** @brief Return whether `close()` has been called. */
  bool is_closed() const override;

 private:
  std::unique_ptr<named_pipe_conn> conn_;
  std::mutex send_mtx_;
  std::mutex recv_mtx_;
};

// ── Client-side transport ─────────────────────────────────────────────────────

/**
 * @brief Client transport that connects to a named pipe / Unix socket.
 *
 * @param path  On Windows: full pipe path (`\\.\pipe\name`).
 *              On Linux/macOS: file-system socket path (`/tmp/wish.sock`).
 */
class named_pipe_client_transport : public client_transport_iface {
 public:
  explicit named_pipe_client_transport(std::string path);
  ~named_pipe_client_transport();

  named_pipe_client_transport(const named_pipe_client_transport&) = delete;
  named_pipe_client_transport& operator=(const named_pipe_client_transport&) = delete;

  /**
   * @brief Connect to the named pipe / Unix socket.
   * @throws std::runtime_error if the connection cannot be established.
   */
  void open(bison::dynamic params) override;

  /**
   * @brief Write one length-prefixed frame to the server.
   * @throws std::runtime_error on write failure.
   */
  void send(bison::buffer frame) override;

  /**
   * @brief Read one length-prefixed frame from the server.
   * @param frame   Output frame buffer.
   * @param timeout Maximum wait before returning `false`.
   * @return `true` on success; `false` on timeout or disconnect.
   */
  bool receive(bison::buffer& frame, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Disconnect; pending receives return `false`. */
  void shutdown() override;

 private:
  std::string path_;
  std::unique_ptr<named_pipe_conn> conn_;
  std::mutex send_mtx_;
  std::mutex recv_mtx_;
};

// ── Server-side transport ─────────────────────────────────────────────────────

/**
 * @brief Named-pipe / Unix-socket server transport.
 *
 * Maintains a listening server endpoint and produces one
 * `named_pipe_server_connection` per `accept()` call.  Multiple concurrent
 * connections are supported.
 *
 * @param path  On Windows: full pipe path (`\\.\pipe\name`).
 *              On Linux/macOS: file-system socket path (`/tmp/wish.sock`).
 */
class named_pipe_server_transport : public server_transport_iface {
 public:
  explicit named_pipe_server_transport(std::string path);
  ~named_pipe_server_transport();

  named_pipe_server_transport(const named_pipe_server_transport&) = delete;
  named_pipe_server_transport& operator=(const named_pipe_server_transport&) = delete;

  /** @brief Open the listening endpoint. */
  void start(bison::dynamic params) override;

  /**
   * @brief Wait for the next client connection.
   * @param timeout  Maximum wait before returning `nullptr`.
   * @return Connection on success; `nullptr` on timeout or stop.
   */
  std::unique_ptr<server_connection_iface> accept(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Stop accepting; pending and future `accept()` calls return `nullptr`. */
  void stop() override;

 private:
  std::string path_;
  std::atomic<bool> stopped_{false};
  std::unique_ptr<named_pipe_server_state> state_;
};

} // namespace bdg::bison::rmi::transport
