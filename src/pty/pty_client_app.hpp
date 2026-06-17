// MIT License © 2025 Binary Dice Games
/**
 * @file pty_client_app.hpp
 * @brief Remote PTY client application scaffold.
 */
#pragma once

#if defined(__linux__)

#include "src/core/bison.hpp"
#include "src/rmi/client/client.hpp"

#include <string>

namespace bdg::bison::pty {

/**
 * @brief Extensible base class for processes that connect to a `pty_server_app`.
 *
 * Runs on the remote machine (e.g., after an `ssh` into the server host).
 * Uses the process's own `stdin`/`stdout` as the bison transport — no
 * subprocess is launched.  The SSH channel carries the DCS frames between
 * this process and the `pty_server_transport` running on the server.
 *
 * Concrete applications override `on_session()` to interact with the
 * server's remote objects (instantiate, call, get/set, etc.).  The base
 * class handles transport construction, handshake, and disconnect.
 *
 * Typical lifecycle:
 * 1. `connect()` via `stdio_client_transport(stdin, stdout)` — waits for the
 *    server's HELLO frame that was pre-emitted by `pty_server_transport`.
 * 2. `on_connected()` hook (default: no-op).
 * 3. `on_session(rmi_client)` — application logic.
 * 4. `disconnect()`.
 *
 * Linux only.
 */
class pty_client_app {
 public:
  virtual ~pty_client_app() = default;

  /**
   * @brief Connect to the server and run the session.
   *
   * @return The value returned by `on_session()`, or 1 on error.
   */
  int run(int argc, char** argv);

 protected:
  /**
   * @brief Main application logic for the RMI session.
   *
   * Called after the handshake completes.  Interact with the server through
   * @p c (instantiate objects, call methods, etc.).  The return value becomes
   * the process exit code.
   *
   * @param c Connected RMI client.
   * @return Exit code.
   */
  virtual int on_session(rmi::client& c) = 0;

  /**
   * @brief Called immediately after the handshake succeeds (default: no-op).
   */
  virtual void on_connected() const;

  /**
   * @brief Called when a transport or session exception is caught.
   * @param msg Human-readable error description.
   */
  virtual void on_error(const std::string& msg) const;

  /**
   * @brief Populate connection parameters before `connect()` is called.
   *
   * Default values: `mode=dcs`, `handshake_timeout_ms=300000` (five minutes,
   * to allow the user time to SSH into the server and start the client
   * before the server-side HELLO window expires).
   *
   * @param params In/out parameter map.
   */
  virtual void on_connect_params(bison::dynamic& params) const;
};

} // namespace bdg::bison::pty

#endif // defined(__linux__)
