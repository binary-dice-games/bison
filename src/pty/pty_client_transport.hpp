// MIT License © 2025 Binary Dice Games
/**
 * @file pty_client_transport.hpp
 * @brief PTY client transport: uses process stdin/stdout as the DCS bison channel.
 *
 * Unlike `stdio_client_transport`, which waits for the server to send HELLO
 * first, `pty_client_transport` initiates the handshake — it emits HELLO on
 * `open()` and then waits for the server's HELLO response.  This matches
 * `pty_server_transport::accept()`, which waits for the client's HELLO before
 * responding with its own.
 *
 * Linux only.
 */
#pragma once

#if defined(__linux__)

#include "src/core/bison.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <chrono>
#include <memory>

namespace bdg::bison::pty {

/**
 * @brief Client-side transport for processes running inside a PTY channel.
 *
 * Uses the process's own `stdin` / `stdout` (the PTY slave or SSH channel)
 * as the bison DCS transport.  Designed to pair with `pty_server_transport`
 * running on the server side.
 *
 * ### Handshake
 *
 * 1. `open()` starts the reader thread, emits a HELLO frame to `stdout`,
 *    then blocks until a HELLO frame arrives from `stdin` or the timeout
 *    elapses.
 * 2. The server (`pty_server_transport::accept()`) detects the client's
 *    HELLO, responds with its own HELLO, and returns the connection.
 * 3. `open()` receives the server's HELLO and returns.
 *
 * Non-DCS bytes on `stdin` (shell prompts, escape sequences) are silently
 * discarded — the client process is not a terminal emulator.
 *
 * Linux only.
 */
class pty_client_transport : public rmi::transport::client_transport_iface {
 public:
  pty_client_transport();
  ~pty_client_transport();

  pty_client_transport(pty_client_transport&&) noexcept;
  pty_client_transport& operator=(pty_client_transport&&) noexcept;

  pty_client_transport(const pty_client_transport&)            = delete;
  pty_client_transport& operator=(const pty_client_transport&) = delete;

  /**
   * @brief Emit HELLO and wait for the server's HELLO response.
   *
   * Starts the reader thread, emits a DCS HELLO frame on `stdout`, then
   * blocks until a HELLO frame arrives on `stdin`.
   *
   * Recognised params:
   *  - `handshake_timeout_ms` (int32) — max wait for server's HELLO
   *    (default: 300 000 ms).
   *
   * @throws std::runtime_error if the handshake times out or the channel
   *         closes before HELLO arrives.
   */
  void open(bison::dynamic params) override;

  /**
   * @brief Encode @p frame as DCS DATA chunks and write them to `stdout`.
   * @throws std::runtime_error if the transport is not open or is closed.
   */
  void send(bison::buffer frame) override;

  /**
   * @brief Block until a DATA frame arrives from the server or timeout elapses.
   * @return `true` when a frame was received; `false` on timeout or close.
   */
  bool receive(
      bison::buffer& frame,
      std::chrono::milliseconds timeout) override;

  /**
   * @brief Emit an END frame and shut down the reader thread.
   */
  void shutdown() override;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace bdg::bison::pty

#endif // defined(__linux__)
