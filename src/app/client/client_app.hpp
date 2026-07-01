// MIT License © 2025 Binary Dice Games
/**
 * @file client_app.hpp
 * @brief Generic multi-transport client application scaffold.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/client/client.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace bdg::bison::app {

/**
 * @brief Extensible base class for bison RMI client applications.
 *
 * Handles command-line transport selection and the connect/session/disconnect
 * lifecycle.  Concrete applications override `on_session()` to drive their
 * domain logic through the connected RMI client.
 *
 * Transport is chosen by gflags CLI flags (all optional):
 *   - `--pty`                   — use PTY transport (stdin/stdout DCS framing);
 *                                 for use when the client is launched inside a
 *                                 `pty_server_transport` child process (Linux/Windows)
 *   - `--pipe PATH`             — named-pipe / Unix-socket path
 *   - `--host HOST --port PORT` — TCP socket (default: `127.0.0.1:7070`)
 *   - `--timeout MS`            — per-request timeout stored in `timeout_`
 *
 * `--pty` takes precedence over `--pipe` and `--host`/`--port`;
 *
 * Lifecycle (inside `run()`):
 * 1. Parse flags.
 * 2. Build the selected transport.
 * 3. Call `run_with_transport(transport)`.
 *   a. `on_connect_params(params)` — populate connection parameters.
 *   b. `c.connect(params)`.
 *   c. `on_connected()`.
 *   d. `on_session(c)` — application logic (pure virtual).
 *   e. `c.disconnect()`.
 * 4. On exception: `on_error(msg)`, return 1.
 *
 * Subclasses that always use a specific transport (e.g. `pty_client_app`)
 * can override `run()` to bypass flag parsing and call `run_with_transport()`
 * directly with the desired transport.
 */
class client_app {
 public:
  virtual ~client_app() = default;

  /**
   * @brief Parse flags, build transport, connect, run session, disconnect.
   *
   * @param argc  Argument count from `main`.
   * @param argv  Argument vector from `main`.
   * @return Value returned by `on_session()`, or 1 on error.
   */
  virtual int run(int argc, char** argv);

 protected:
  /**
   * @brief Main application logic for the RMI session.
   *
   * Called after the transport is connected.  The return value becomes the
   * return value of `run()`.
   *
   * @param c Connected RMI client.
   * @return Exit code.
   */
  virtual int on_session(rmi::client& c) = 0;

  /**
   * @brief Called immediately after the connection handshake succeeds.
   *
   * Default: no-op.
   */
  virtual void on_connected() const {}

  /**
   * @brief Populate connection parameters before `connect()` is called.
   *
   * Default: sets `timeout_ms` to the value of `FLAGS_timeout` (or 30 000 if
   * not parsed yet).  Called after flag parsing, so FLAGS values are available.
   *
   * @param params In/out parameter map.
   */
  virtual void on_connect_params(bison::dynamic& params) const;

  /**
   * @brief Called when a transport or session exception is caught.
   *
   * Default: writes to `std::cerr`.
   *
   * @param msg Human-readable error description.
   */
  virtual void on_error(const std::string& msg) const;

  /**
   * @brief Take ownership of @p transport, connect, call hooks, run session,
   *        then disconnect.
   *
   * Subclasses that control transport construction (e.g. `pty_client_app`)
   * call this directly instead of going through `run()`.
   *
   * Does NOT catch exceptions — the caller (`run()` or the subclass override)
   * is responsible for catching and routing them to `on_error()`.
   *
   * @param transport  Heap-allocated transport to take ownership of.
   * @return Return value of `on_session()`.
   */
  int run_with_transport(std::unique_ptr<rmi::transport::client_transport_iface> transport);

  /** @brief Per-request timeout; set from `--timeout` before `on_session()`. */
  std::chrono::milliseconds timeout_{30000};
};

} // namespace bdg::bison::app
