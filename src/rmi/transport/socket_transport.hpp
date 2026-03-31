// MIT License © 2025 Binary Dice Games
/**
 * @file socket_transport.hpp
 * @brief TCP socket transport implementation for RMI.
 */
#pragma once

#include "src/core/bison.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace bdg::bison::rmi::transport {

/**
 * @brief Client-side TCP transport endpoint.
 */
class socket_client_transport {
 public:
    /** @brief Construct a client endpoint with default host/port values. */
  socket_client_transport(std::string host = "127.0.0.1", uint16_t port = 7070);
    /** @brief Close the endpoint if still open. */
  ~socket_client_transport();

  socket_client_transport(socket_client_transport&&) noexcept;
  socket_client_transport& operator=(socket_client_transport&&) noexcept;

  socket_client_transport(const socket_client_transport&) = delete;
  socket_client_transport& operator=(const socket_client_transport&) = delete;

    /**
     * @brief Open and connect to the configured TCP endpoint.
     * @param params Optional overrides (for example host/port fields).
     */
  void open(bison::dynamic&& params);

    /** @brief Send one framed message to the server. */
  void send(bison::buffer frame);

    /**
     * @brief Receive one framed message from the server.
     * @param frame Output frame buffer.
     * @param timeout Maximum wait duration before returning `false`.
     * @return `true` when a frame was received, otherwise `false`.
     */
  bool receive(
      bison::buffer& frame,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

    /** @brief Shutdown and close the socket endpoint. */
  void shutdown();

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

/**
 * @brief Server-side TCP connection accepted from a listener.
 */
class socket_server_connection {
 public:
    /** @brief Construct an empty/closed connection state. */
  socket_server_connection();
    /** @brief Close the connection if still active. */
  ~socket_server_connection();

  socket_server_connection(socket_server_connection&&) noexcept;
  socket_server_connection& operator=(socket_server_connection&&) noexcept;

  socket_server_connection(const socket_server_connection&) = delete;
  socket_server_connection& operator=(const socket_server_connection&) = delete;

    /** @brief Send one framed message to the connected client. */
  void send(bison::buffer frame);

    /**
     * @brief Receive one framed message from the connected client.
     * @param frame Output frame buffer.
     * @param timeout Maximum wait duration before returning `false`.
     * @return `true` when a frame was received, otherwise `false`.
     */
  bool receive(
      bison::buffer& frame,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

    /** @brief Close this accepted connection. */
  void close();

    /** @brief Return whether the connection is closed. */
  bool is_closed() const;

 private:
  friend class socket_server_transport;
  struct impl;
  explicit socket_server_connection(std::unique_ptr<impl> impl);

  std::unique_ptr<impl> impl_;
};

/**
 * @brief TCP listener transport for server-side RMI endpoints.
 */
class socket_server_transport {
 public:
    /** @brief Construct a listener using default bind host/port values. */
  socket_server_transport(
      std::string bind_host = "127.0.0.1",
      uint16_t port = 7070);
    /** @brief Stop listening and release listener resources. */
  ~socket_server_transport();

  socket_server_transport(socket_server_transport&&) noexcept;
  socket_server_transport& operator=(socket_server_transport&&) noexcept;

  socket_server_transport(const socket_server_transport&) = delete;
  socket_server_transport& operator=(const socket_server_transport&) = delete;

    /**
     * @brief Start listening for incoming client TCP connections.
     * @param params Optional overrides (for example host/port fields).
     */
  void start(bison::dynamic params);

    /** @brief Create a client transport configured for this listener endpoint. */
  socket_client_transport connect() const;

    /**
     * @brief Accept an incoming client connection.
     * @param timeout Maximum wait duration for the next connection.
     * @return Accepted server connection on success, otherwise `std::nullopt`.
     */
  std::optional<socket_server_connection> accept(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

    /** @brief Stop listening for new connections. */
  void stop();

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace bdg::bison::rmi::transport
