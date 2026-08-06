// MIT License © 2025 Binary Dice Games
/**
 * @file server_app.hpp
 * @brief Extensible server application scaffold for bison RMI servers.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/server/context.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <memory>
#include <string>

namespace bdg::bison::rmi {
class server;
} // namespace bdg::bison::rmi

namespace bdg::bison::term {
class terminal;
} // namespace bdg::bison::term

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
 *                       `tls`, or `term`. Only the flags relevant to the
 *                       selected transport are read; the others are simply
 *                       ignored (see `src/app/transport_flags.hpp`).
 *   - `--host HOST`     bind host for `--transport=tcp`/`tls` (default: `0.0.0.0`)
 *   - `--port PORT`     listen port for `--transport=tcp`/`tls` (default: `7070`)
 *   - `--name PATH`     named-pipe / Unix-socket path, used by
 *                       `--transport=pipe`
 *   - `--cert_file`/`--cert_pem`, `--key_file`/`--key_pem`, `--key_password`
 *                       server certificate/key, required for `--transport=tls`
 *                       (see `docs/tls.md`)
 *   - `--client_auth`   `none` (default) | `optional` | `required` -- mutual
 *                       TLS mode, for `--transport=tls`
 *   - `--ca_file`/`--ca_pem` trust anchor for verifying client certificates,
 *                       required when `--client_auth` is not `none`
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
   * @brief Short label used to distinguish a `--transport=term` spawned
   *        terminal's shell prompt (e.g. "wish-server") from the operator's
   *        own terminal.
   *
   * Default: returns an empty string (no prompt override; the spawned
   * shell's own default prompt is left untouched). See
   * `term::terminal`'s `prompt_label` parameter.
   *
   * @return Short prompt label, or empty for no override.
   */
  virtual std::string terminal_label() const {
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
   * @brief Construct the RMI server bound to `transport`.
   *
   * Override to return a subclass of `rmi::server` (e.g. one with its own
   * render loop) so the default `run_with_transport()` -- or an override
   * that still wants app-hook forwarding -- can use it. Default: an internal
   * server that forwards `on_session_created`/`on_session_destroyed`/
   * `on_verbose_trace`/`server_description` to this app's hooks.
   *
   * @param transport  Transport the server will accept connections over.
   * @return Newly constructed server, bound to @p transport.
   */
  virtual std::unique_ptr<rmi::server> make_server(rmi::transport::server_transport_iface& transport);

  /**
   * @brief Populate transport-specific listen parameters before `srv->listen()`.
   *
   * Default: no-op unless `--transport=tls` is selected, in which case
   * populates `cert_file`/`cert_pem`/`key_file`/`key_pem`/`key_password`/
   * `client_auth`/`ca_file`/`ca_pem` from the corresponding `FLAGS_*` (see
   * `tls_socket_server_transport::start()` in `docs/tls.md`). Called from
   * `run_with_transport()` after flag parsing, so `FLAGS_*` values are
   * available. Mirrors `client_app::on_connect_params()`.
   *
   * @param params In/out parameter map, forwarded to `transport.start()` via
   *               `rmi::server::listen()`.
   */
  virtual void on_listen_params(bison::dynamic& params) const;

  /**
   * @brief Create a server using `transport` and block until shutdown.
   *
   * Override to substitute a different server type (e.g. one with a GUI
   * render loop). The default creates a `bridged_server`, calls
   * `on_listen_params()` to build the params passed to `listen()`, and
   * blocks on `wait_for_shutdown()`.
   *
   * Host and port are not passed as parameters — they are available through
   * `FLAGS_host` and `FLAGS_port`, which are defined in the binary's
   * `main.cpp` and carry meaning only for `--transport=tcp`/`tls`.
   *
   * @param transport  Bound transport to serve.
   * @return 0 on clean shutdown, non-zero on error.
   */
  virtual int run_with_transport(rmi::transport::server_transport_iface& transport);

  /**
   * @brief Block until the server should stop.
   *
   * Default: waits on `active_term_` (the spawned terminal exiting) when
   * `--transport=term` selected it, otherwise waits for a line on stdin
   * (read on a helper thread so this can still be interrupted). Stdin is
   * unavailable as a shutdown trigger under `--transport=term` since it is
   * being pumped into the spawned terminal instead. Either way, also
   * returns as soon as `SIGINT`/`SIGTERM` is received, so `run()`'s caller
   * reaches `srv->stop()` (which closes every connected client's session)
   * instead of the process dying outright via the OS's default signal
   * action.
   */
  virtual void wait_for_shutdown();

  /**
   * @brief Non-blocking check for whether the server should stop.
   *
   * Default: true once `active_term_` has exited (`--transport=term`) or
   * `SIGINT`/`SIGTERM` has been received, false otherwise. Intended for
   * overrides (e.g. a GUI-driven `run_with_transport`) that poll their own
   * shutdown condition (a window close, etc.) in a loop and need to check
   * for the spawned terminal's exit and pending shutdown signals alongside
   * it rather than blocking on `wait_for_shutdown()`.
   */
  virtual bool is_shutdown_requested() const;

  /**
   * @brief Non-owning pointer to the spawned terminal when
   *        `--transport=term` selected it, set by `run()` for the
   *        duration of the term-transport `run_with_transport()` call;
   *        null otherwise. Read by the default `wait_for_shutdown()` and
   *        `is_shutdown_requested()`.
   */
  term::terminal* active_term_ = nullptr;
};

} // namespace bdg::bison::app
