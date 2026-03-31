// MIT License © 2025 Binary Dice Games
/**
 * @file stdio_transport.hpp
 * @brief Stdio stream transport implementation for RMI.
 */
#pragma once

#include "src/core/bison.hpp"

#include <chrono>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>

namespace bdg::bison::rmi::transport {

/**
 * @brief Client-side endpoint using stdin/stdout as a framed channel.
 */
class stdio_client_transport {
 public:
  /** @brief Construct using process stdin/stdout streams. */
  stdio_client_transport();

  /**
   * @brief Construct using explicit input/output streams.
   *
   * This overload is primarily useful for tests.
   */
  stdio_client_transport(std::istream& in, std::ostream& out);

#if defined(__linux__)
  /**
   * @brief Construct using explicit file descriptors.
   *
   * This is intended for PTY-backed workflows on Linux/WSL.
   */
  stdio_client_transport(int read_fd, int write_fd);
#endif

  /** @brief Release transport resources. */
  ~stdio_client_transport();

  stdio_client_transport(stdio_client_transport&&) noexcept;
  stdio_client_transport& operator=(stdio_client_transport&&) noexcept;

  stdio_client_transport(const stdio_client_transport&) = delete;
  stdio_client_transport& operator=(const stdio_client_transport&) = delete;

  /**
   * @brief Open and handshake the stdio channel.
   * @param params Optional transport configuration.
   */
  void open(bison::dynamic&& params);

  /** @brief Send one encoded frame to the remote endpoint. */
  void send(bison::buffer frame);

  /**
   * @brief Receive one encoded frame from the remote endpoint.
   * @return `true` when a frame was received, otherwise `false`.
   */
  bool receive(
      bison::buffer& frame,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

  /** @brief Close transport and send an end-of-session control message. */
  void shutdown();

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

/**
 * @brief Server-side connection over stdin/stdout framed channel.
 */
class stdio_server_connection {
 public:
  /** @brief Construct an empty/closed connection state. */
  stdio_server_connection();

  /** @brief Close connection resources. */
  ~stdio_server_connection();

  stdio_server_connection(stdio_server_connection&&) noexcept;
  stdio_server_connection& operator=(stdio_server_connection&&) noexcept;

  stdio_server_connection(const stdio_server_connection&) = delete;
  stdio_server_connection& operator=(const stdio_server_connection&) = delete;

  /** @brief Send one encoded frame to the remote endpoint. */
  void send(bison::buffer frame);

  /**
   * @brief Receive one encoded frame from the remote endpoint.
   * @return `true` when a frame was received, otherwise `false`.
   */
  bool receive(
      bison::buffer& frame,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

  /** @brief Close this connection. */
  void close();

  /** @brief Return whether this connection has been closed. */
  bool is_closed() const;

 private:
  friend class stdio_server_transport;
  struct impl;
  explicit stdio_server_connection(std::unique_ptr<impl> impl);

  std::unique_ptr<impl> impl_;
};

/**
 * @brief Server-side listener over stdin/stdout framed channel.
 */
class stdio_server_transport {
 public:
  /** @brief Construct using process stdin/stdout streams. */
  stdio_server_transport();

  /**
   * @brief Construct using explicit input/output streams.
   *
   * This overload is primarily useful for tests.
   */
  stdio_server_transport(std::istream& in, std::ostream& out);

  /** @brief Stop and release listener resources. */
  ~stdio_server_transport();

  stdio_server_transport(stdio_server_transport&&) noexcept;
  stdio_server_transport& operator=(stdio_server_transport&&) noexcept;

  stdio_server_transport(const stdio_server_transport&) = delete;
  stdio_server_transport& operator=(const stdio_server_transport&) = delete;

  /**
   * @brief Start the framed stdio listener and emit a hello control frame.
   * @param params Optional transport configuration.
   */
  void start(bison::dynamic params);

  /**
   * @brief Accept the single stdio connection.
   * @return Connection on first accept, otherwise `std::nullopt`.
   */
  std::optional<stdio_server_connection> accept(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

  /** @brief Stop the listener and close the active connection state. */
  void stop();

  /**
   * @brief Wait until the underlying stream channel is closed.
   * @return `true` when closed, otherwise `false` on timeout.
   */
  bool wait_until_closed(std::chrono::milliseconds timeout) const;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace bdg::bison::rmi::transport
