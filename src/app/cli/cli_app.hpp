// MIT License © 2025 Binary Dice Games
/**
 * @file cli_app.hpp
 * @brief Interactive REPL application scaffold for bison RMI servers.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/client/client.hpp"

#include <chrono>
#include <string>

namespace bdg::bison::app {

/**
 * @brief Extensible base class for interactive bison CLI applications.
 *
 * Parses command-line flags, constructs the appropriate transport, and
 * manages the connect/session/disconnect lifecycle.  Concrete applications
 * override `on_session()` to replace the built-in REPL with custom
 * behaviour (for example, a terminal UI driven by remote objects).
 *
 * Default `on_session()` runs a scripting-style REPL:
 *
 * @code
 * > t = instantiate("Ikea", "Table")
 * > t.get({"material": null})
 * { "material": "wood" }
 * > t.set({"material": "iron"})
 * > t.call("flip", {})
 * > del t
 * > exit
 * @endcode
 *
 * Transport is selected by CLI flags (exactly one must be given):
 *   - (default) `--host HOST --port PORT` – TCP socket (default localhost:7070)
 *   - `--pty`   – PTY transport (Linux only)
 *
 * Cross-platform (Windows, Linux, macOS); PTY transport is Linux-only.
 */
class cli_app {
 public:
  virtual ~cli_app() = default;

  /**
   * @brief Parse flags, connect, run session, disconnect.
   *
   * @param argc  Argument count from `main`.
   * @param argv  Argument vector from `main`.
   * @return Value returned by `on_session()`, or 1 on error.
   */
  int run(int argc, char** argv);

 protected:
  /**
   * @brief Main application logic for the RMI session.
   *
   * Called after the transport is connected.  The default implementation
   * runs the interactive scripting-style REPL.  Override to replace it
   * entirely (for example, with a full-screen terminal UI).
   *
   * The `timeout_` member is set from `--timeout` before this is called.
   *
   * @param c Connected RMI client.
   * @return Exit code; becomes the return value of `run()`.
   */
  virtual int on_session(rmi::client& c);

  /**
   * @brief Called immediately after the connection handshake succeeds.
   *
   * Default: no-op.
   */
  virtual void on_connected() const;

  /**
   * @brief Populate connection parameters before `connect()` is called.
   *
   * Default: sets `timeout_ms` to 30 000 (30 s).
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

  /** @brief Per-request timeout applied to all blocking futures in `on_session()`. */
  std::chrono::milliseconds timeout_{30000};
};

} // namespace bdg::bison::app
