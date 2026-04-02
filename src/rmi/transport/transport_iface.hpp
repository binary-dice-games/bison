// MIT License © 2025 Binary Dice Games
/**
 * @file transport_iface.hpp
 * @brief Public transport interface types shared by client and server.
 *
 * Concrete transport implementations (`socket_*`, `memory_*`, `stdio_*`)
 * inherit from these interfaces.  `client` and `server` accept
 * `unique_ptr<client_transport_iface>` and `unique_ptr<server_transport_iface>`
 * respectively, which makes them independent of the concrete transport type
 * at compile time and enables the C ABI to use a single set of functions
 * for any supported transport.
 */
#pragma once

#include "src/core/bison.hpp"

#include <chrono>
#include <memory>

namespace bdg::bison::rmi::transport {

// ── Forward declarations ──────────────────────────────────────────────────────

struct server_connection_iface;

// ── Client-side transport interface ──────────────────────────────────────────

/**
 * @brief Abstract interface for client-side transport endpoints.
 *
 * Implementations must provide frame-level send/receive semantics over any
 * underlying channel (TCP socket, stdio pipe, in-memory queue, …).
 *
 * @par Ownership
 * `client` holds a `unique_ptr<client_transport_iface>` and is the sole
 * owner of the transport object throughout the client's lifetime.
 */
struct client_transport_iface {
  /**
   * @brief Open and connect the transport channel.
   * @param params Optional transport-specific parameters.
   */
  virtual void open(bison::dynamic params) = 0;

  /**
   * @brief Send one encoded frame to the remote endpoint.
   * @param frame Encoded frame bytes (consumed by move).
   */
  virtual void send(bison::buffer frame) = 0;

  /**
   * @brief Receive one encoded frame from the remote endpoint.
   * @param frame Output frame buffer.
   * @param timeout Maximum wait duration before returning `false`.
   * @return `true` when a frame was received; otherwise `false`.
   */
  virtual bool receive(bison::buffer& frame, std::chrono::milliseconds timeout) = 0;

  /** @brief Shutdown and close the transport channel. */
  virtual void shutdown() = 0;

  virtual ~client_transport_iface() = default;
};

// ── Server-side connection interface ─────────────────────────────────────────

/**
 * @brief Abstract interface for one accepted server-side connection.
 *
 * Each call to `server_transport_iface::accept()` produces one instance.
 * The `server` owns the returned `unique_ptr` for the lifetime of the
 * corresponding worker thread.
 */
struct server_connection_iface {
  /**
   * @brief Send one encoded frame to the connected client.
   * @param frame Encoded frame bytes (consumed by move).
   */
  virtual void send(bison::buffer frame) = 0;

  /**
   * @brief Receive one encoded frame from the connected client.
   * @param frame Output frame buffer.
   * @param timeout Maximum wait duration before returning `false`.
   * @return `true` when a frame was received; otherwise `false`.
   */
  virtual bool receive(bison::buffer& frame, std::chrono::milliseconds timeout) = 0;

  /** @brief Close this connection. */
  virtual void close() = 0;

  /** @brief Return whether this connection has been closed. */
  virtual bool is_closed() const = 0;

  virtual ~server_connection_iface() = default;
};

// ── Server-side transport interface ──────────────────────────────────────────

/**
 * @brief Abstract interface for server-side transport listeners.
 *
 * An implementation accepts incoming connections and hands each one back
 * as a `unique_ptr<server_connection_iface>`.  `server` holds a pointer to
 * this interface (either owning or borrowed) and drives the accept loop.
 */
struct server_transport_iface {
  /**
   * @brief Start the listener and prepare to accept connections.
   * @param params Optional transport-specific listen parameters.
   */
  virtual void start(bison::dynamic params) = 0;

  /**
   * @brief Accept the next incoming connection.
   *
   * @param timeout Maximum wait duration for the next connection.
   * @return A live connection on success, or `nullptr` on timeout.
   */
  virtual std::unique_ptr<server_connection_iface> accept(
      std::chrono::milliseconds timeout) = 0;

  /** @brief Stop the listener and close all pending accept operations. */
  virtual void stop() = 0;

  virtual ~server_transport_iface() = default;
};

} // namespace bdg::bison::rmi::transport
