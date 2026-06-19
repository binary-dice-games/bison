// MIT License © 2025 Binary Dice Games
/**
 * @file srv_app.hpp
 * @brief Extensible server application scaffold for bison RMI servers.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/server/context.hpp"

#include <string>

namespace bdg::bison::app {

/**
 * @brief Extensible base class for bison RMI server applications.
 *
 * Handles command-line transport selection, class registration, and the
 * listen/stop lifecycle.  Concrete server applications override
 * `register_classes()` to populate the bison class registry, then call
 * `run()` from `main()`.
 *
 * Supported gflags CLI flags:
 *   - `--host HOST`  bind host (default: `0.0.0.0`)
 *   - `--port PORT`  listen port (default: `7070`)
 *
 * Run with `--help` to see all available flags.
 *
 * Typical lifecycle:
 * 1. `register_classes()` — populate the bison class registry.
 * 2. Start TCP listener on the configured host and port.
 * 3. Call `on_listening()` (default: prints to stdout).
 * 4. Accept connections and serve requests until Enter is pressed.
 * 5. Stop the listener and clean up.
 */
class srv_app {
 public:
  virtual ~srv_app() = default;

  /**
   * @brief Parse flags, register classes, start server, and block until done.
   *
   * @param argc  Argument count from `main`.
   * @param argv  Argument vector from `main`.
   * @return 0 on clean shutdown, 1 on error.
   */
  int run(int argc, char** argv);

  /**
   * @brief Called after the server starts listening for connections.
   *
   * Default: prints a ready message to stdout.
   *
   * @param host  Bound hostname or IP.
   * @param port  Bound port number.
   */
  virtual void on_listening(const std::string& host, uint16_t port) const;

  /**
   * @brief Called when a new client session is established.
   *
   * Fires in the session worker thread after the context is registered.
   * Default: no-op.
   *
   * @param ctx  Newly created session context.
   */
  virtual void on_session_created(rmi::context& ctx) const;

  /**
   * @brief Called just before a client session is torn down.
   *
   * Fires in the session worker thread before the object table is cleaned up.
   * Default: no-op.
   *
   * @param ctx  Session context about to be destroyed.
   */
  virtual void on_session_destroyed(rmi::context& ctx) const;

  /**
   * @brief Called on fatal errors before `run()` returns 1.
   *
   * Default: writes to `std::cerr`.
   *
   * @param msg  Human-readable error description.
   */
  virtual void on_error(const std::string& msg) const;

#if defined(__linux__)
  /**
   * @brief Called after the PTY server starts (Linux PTY transport only).
   *
   * Default: prints a ready message to stdout.
   */
  virtual void on_listening_pty() const;
#endif

  /**
   * @brief Return an optional preamble for `OP_HELP` responses.
   *
   * The returned string is prepended to the auto-generated class listing in
   * `OP_HELP` responses.  Default: returns an empty string (no preamble).
   *
   * @return Free-form text describing the server's purpose.
   */
  virtual std::string server_description() const { return {}; }

 protected:
  /**
   * @brief Register domain classes in the bison class registry.
   *
   * Called once before the server starts listening.  Use
   * `bison::dynamic::addClass()` to register prototype objects.
   */
  virtual void register_classes() = 0;
};

} // namespace bdg::bison::app
