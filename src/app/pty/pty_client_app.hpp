// MIT License © 2025 Binary Dice Games
/**
 * @file pty_client_app.hpp
 * @brief PTY client application scaffold — thin wrapper over client_app.
 */
#pragma once

#include "src/app/client/client_app.hpp"

namespace bdg::bison::app {

/**
 * @brief Extensible base class for processes that connect to a `pty_server_app`.
 *
 * `pty_client_app` is a thin subclass of `client_app` that always uses the
 * PTY DCS transport (process stdin/stdout) and bypasses flag parsing.
 *
 * Runs on the remote machine (e.g. after an `ssh` into the server host).
 * The SSH channel carries the DCS frames between this process and the
 * `pty_server_transport` on the server.
 *
 * Concrete applications override `on_session()` to drive the RMI session
 * (instantiate objects, call methods, etc.).  All other hooks are inherited
 * from `client_app`:
 * - `on_connected()` — default: no-op
 * - `on_connect_params()` — sets PTY-specific defaults (handshake_timeout_ms)
 * - `on_error()` — default: stderr with `[pty_client_app]` prefix
 *
 * Linux and Windows.
 */
class pty_client_app : public client_app {
 public:
  /**
   * @brief Connect to the PTY server and run the session.
   *
   * Bypasses flag parsing and uses `pty_client_transport` directly.
   * argc/argv are ignored.
   *
   * @return Value returned by `on_session()`, or 1 on error.
   */
  int run(int argc, char** argv) override;

 protected:
  /**
   * @brief Populate PTY-specific connection parameters.
   *
   * Sets `handshake_timeout_ms` to 300 000 (five minutes) to allow the user
   * time to SSH in and start the client before the server's HELLO window
   * expires.  Override to adjust the timeout.
   */
  void on_connect_params(bison::dynamic& params) const override;

  /**
   * @brief Called when a transport or session exception is caught.
   *
   * Default: writes to `std::cerr` with a `[pty_client_app]` prefix.
   */
  void on_error(const std::string& msg) const override;
};

} // namespace bdg::bison::app
