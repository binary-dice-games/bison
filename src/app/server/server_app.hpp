// MIT License © 2025 Binary Dice Games
/**
 * @file server_app.hpp
 * @brief Extensible server application scaffold for bison RMI servers.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/server/context.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <functional>
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
 *   - `--transport T`   selects the transport: `tcp` (default), `pipe`,
 *                       `pty`, or `console`. Only the flags relevant to the
 *                       selected transport are read; the others are simply
 *                       ignored (see `src/app/transport_flags.hpp`).
 *   - `--host HOST`     bind host for `--transport=tcp` (default: `0.0.0.0`)
 *   - `--port PORT`     listen port for `--transport=tcp` (default: `7070`)
 *   - `--name PATH`     named-pipe / Unix-socket path, used by
 *                       `--transport=pipe`
 *   - `--verbose`       print one request trace line and one response trace
 *                       line per RMI operation to stdout (open, connect,
 *                       instantiate, call, get, set, destroy, disconnect, …)
 *
 * Run with `--help` to see all available flags.
 *
 * Typical lifecycle:
 * 1. `register_classes()` — populate the bison class registry.
 * 2. Start TCP listener on the configured host and port (or spawn a pty).
 * 3. Call `on_listening()` (default: prints to stdout).
 * 4. Accept connections and serve requests until Enter is pressed (socket /
 *    named-pipe transports) or the spawned shell exits (`--transport=pty`).
 * 5. Stop the listener and clean up.
 */
class server_app {
 public:
  virtual ~server_app() = default;

  /**
   * @brief Parse flags, register classes, start server, and block until done.
   *
   * @param argc  Argument count from `main`.
   * @param argv  Argument vector from `main`.
   * @return 0 on clean shutdown, 1 on error.
   */
  virtual int run(int argc, char** argv);

  /**
   * @brief Called after the server starts listening for connections.
   *
   * Default: prints a ready message to stdout.  The default implementation
   * uses `FLAGS_host` and `FLAGS_port` to report the socket address (or, in
   * `--transport=pty` mode, writes directly to fd 1 with `\r\n` line endings
   * instead.
   */
  virtual void on_listening() const;

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
   * @param msg  Human-readable error description.
   */
  virtual void on_error(const std::string& msg) const;

  /**
   * @brief Return an optional preamble for `OP_HELP` responses.
   *
   * The returned string is prepended to the auto-generated class listing in
   * `OP_HELP` responses.  Default: returns an empty string (no preamble).
   *
   * @return Free-form text describing the server's purpose.
   */
  virtual std::string server_description() const {
    return {};
  }

  /**
   * @brief Called once per formatted verbose trace line when `--verbose` is
   *        active.
   *
   * The default implementation writes @p line followed by `'\n'` to
   * `std::cout` (or, in `--transport=pty` mode, directly to fd 1 with `\r\n`
   * line endings — see `on_listening()`).  Override to redirect verbose
   * output (e.g. to a log file).
   *
   * @param session_id  Session that generated the trace event.
   * @param line        Formatted trace message (no trailing newline).
   */
  virtual void on_verbose_trace(bison::key_t session_id, const std::string& line) const;

 protected:
  /**
   * @brief Register domain classes in the bison class registry.
   *
   * Called once before the server starts listening.  Use
   * `bison::dynamic::addClass()` to register prototype objects.
   */
  virtual void register_classes() = 0;

  /**
   * @brief Create a server using `transport` and block until shutdown.
   *
   * Override to substitute a different server type (e.g. one with a GUI
   * render loop).  The default creates a `bridged_server` and blocks on
   * `wait_for_shutdown` (or, if unset, on stdin until Enter is pressed).
   *
   * Host and port are not passed as parameters — they are available through
   * `FLAGS_host` and `FLAGS_port`, which are defined in the binary's
   * `main.cpp` and carry meaning only for `--transport=tcp`.
   *
   * @param transport  Bound transport to serve.
   * @param wait_for_shutdown  Blocks until the server should stop. Default
   *        (empty function): `std::getline(std::cin, line)`. `--transport=pty`
   *        passes `[&]{ pty.wait(); }` instead, since stdin is being pumped
   *        into the pty and is not available for a shutdown keypress.
   *        `--transport=console` similarly passes `[&]{ console_proc.wait(); }`,
   *        so the server stops as soon as the spawned subprocess exits.
   * @param is_shutdown_requested  Non-blocking alternative to
   *        `wait_for_shutdown`, for overrides (e.g. a GUI-driven
   *        `run_with_transport`) that poll their own shutdown condition
   *        (a window close, etc.) in a loop and need to check for the
   *        spawned terminal's exit alongside it rather than blocking on it.
   *        `--transport=term` passes `[&]{ return term_proc.has_exited(); }`;
   *        other transports leave this unset. The default implementation
   *        ignores it and blocks on `wait_for_shutdown` as described above.
   * @return 0 on clean shutdown, non-zero on error.
   */
  virtual int run_with_transport(
      rmi::transport::server_transport_iface& transport,
      std::function<void()> wait_for_shutdown = nullptr,
      std::function<bool()> is_shutdown_requested = nullptr);
};

} // namespace bdg::bison::app
