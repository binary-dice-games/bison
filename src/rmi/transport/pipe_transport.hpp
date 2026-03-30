// MIT License © 2025 Binary Dice Games
/**
 * @file pipe_transport.hpp
 * @brief Named pipe transport implementation for RMI.
 */
#pragma once

#include "src/core/bison.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace bdg::bison::rmi::transport {

/**
 * @brief Client-side named pipe transport endpoint.
 */
class pipe_client_transport {
 public:
  /** @brief Construct a client endpoint with a default pipe name. */
  explicit pipe_client_transport(std::string pipe_name = "\\\\.\\pipe\\bison_rmi");
  /** @brief Close the endpoint if still open. */
  ~pipe_client_transport();

  pipe_client_transport(pipe_client_transport&&) noexcept;
  pipe_client_transport& operator=(pipe_client_transport&&) noexcept;

  pipe_client_transport(const pipe_client_transport&) = delete;
  pipe_client_transport& operator=(const pipe_client_transport&) = delete;

  /**
   * @brief Open and connect to the configured named pipe.
   * @param params Optional overrides (for example pipe/name fields).
   */
  void open(bison::dynamic&& params);

  /** @brief Send one framed message to the server side. */
  void send(bison::buffer frame);

  /**
   * @brief Receive one framed message from the server side.
   * @param frame Output frame buffer.
   * @param timeout Maximum wait duration before returning `false`.
   * @return `true` when a frame was received, otherwise `false`.
   */
  bool receive(
      bison::buffer& frame,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

  /** @brief Shutdown and close the named pipe handle. */
  void shutdown();

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

/**
 * @brief Server-side named pipe connection accepted from a listener.
 */
class pipe_server_connection {
 public:
  /** @brief Construct an empty/closed connection state. */
  pipe_server_connection();
  /** @brief Close the connection if still active. */
  ~pipe_server_connection();

  pipe_server_connection(pipe_server_connection&&) noexcept;
  pipe_server_connection& operator=(pipe_server_connection&&) noexcept;

  pipe_server_connection(const pipe_server_connection&) = delete;
  pipe_server_connection& operator=(const pipe_server_connection&) = delete;

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
  friend class pipe_server_transport;
  struct impl;
  explicit pipe_server_connection(std::unique_ptr<impl> impl);

  std::unique_ptr<impl> impl_;
};

/**
 * @brief Named pipe listener transport for server-side RMI endpoints.
 */
class pipe_server_transport {
 public:
  /** @brief Construct a listener using a default pipe name. */
  explicit pipe_server_transport(std::string pipe_name = "\\\\.\\pipe\\bison_rmi");
  /** @brief Stop listening and release listener resources. */
  ~pipe_server_transport();

  pipe_server_transport(pipe_server_transport&&) noexcept;
  pipe_server_transport& operator=(pipe_server_transport&&) noexcept;

  pipe_server_transport(const pipe_server_transport&) = delete;
  pipe_server_transport& operator=(const pipe_server_transport&) = delete;

  /**
   * @brief Start listening for incoming named pipe client connections.
   * @param params Optional overrides (for example pipe/name fields).
   */
  void start(bison::dynamic params);

  /** @brief Create a client transport configured for this listener endpoint. */
  pipe_client_transport connect() const;

  /**
   * @brief Accept an incoming client connection.
   * @param timeout Maximum wait duration for the next connection.
   * @return Accepted server connection on success, otherwise `std::nullopt`.
   */
  std::optional<pipe_server_connection> accept(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

  /** @brief Stop listening for new connections. */
  void stop();

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace bdg::bison::rmi::transport
