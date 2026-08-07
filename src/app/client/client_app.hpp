// MIT License © 2025 Binary Dice Games
/**
 * @file client_app.hpp
 * @brief Generic multi-transport client application scaffold.
 */
#pragma once

#include "src/app/client/line_editor.hpp"
#include "src/bison/bison.hpp"
#include "src/rmi/client/client.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <string_view>

namespace bdg::bison::app {

/**
 * @brief Extensible base class for bison RMI client applications.
 *
 * Handles command-line transport selection and the connect/session/disconnect
 * lifecycle.  Concrete applications override `on_session()` to drive their
 * domain logic through the connected RMI client.
 *
 * Transport is chosen by gflags CLI flags:
 *   - `--transport T`           — selects the transport: `tcp` (default),
 *                                 `pipe`, `tls`, or `term`. Only the flags
 *                                 relevant to the selected transport are
 *                                 read; the others are simply ignored (see
 *                                 `src/app/transport_flags.hpp`).
 *   - `--host HOST --port PORT` — TCP socket, for `--transport=tcp`/`tls`
 *                                 (default: `127.0.0.1:7070`)
 *   - `--name PATH`             — named-pipe / Unix-socket path, used by
 *                                 `--transport=pipe`
 *   - `--ca_file`/`--ca_pem`    — trust anchor for verifying the server's
 *                                 certificate, for `--transport=tls`
 *                                 (required unless `--insecure_skip_verify`)
 *   - `--insecure_skip_verify`  — skip server certificate verification
 *                                 entirely; unsafe for production, a dev/test
 *                                 escape hatch for self-signed certs
 *   - `--server_name`           — SNI / hostname-verification target for
 *                                 `--transport=tls` (defaults to `--host`)
 *   - `--cert_file`/`--cert_pem`, `--key_file`/`--key_pem`, `--key_password`
 *                                 — optional client certificate/key, used
 *                                 only for mutual TLS (`--transport=tls`)
 *   - `--transport=term`        — same fd 0/1 wrapping and raw-mode/CRLF
 *                                 handling as `--transport=pty`, but framed
 *                                 as OSC-99 escape sequences instead of
 *                                 `BISON<...>` (see
 *                                 `src/rmi/transport/term_transport.hpp`).
 *                                 Client side of `server_app`'s
 *                                 `--transport=term`; no additional flags.
 *   - `--timeout MS`            — per-request timeout stored in `timeout_`
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
 * Subclasses that always use a specific transport can override `run()` to
 * bypass flag parsing and call `run_with_transport()` directly with the
 * desired transport.
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
   * @brief Construct the RMI client used for the session.
   *
   * Override to return a subclass of `rmi::client` (e.g. one that adds
   * domain-specific convenience methods) so `on_session()` can access it via
   * `static_cast` on the reference it's given.  Default: a plain `rmi::client`
   * wrapping @p transport.
   *
   * @param transport Heap-allocated transport to take ownership of.
   * @return Newly constructed client, owning @p transport.
   */
  virtual std::unique_ptr<rmi::client> make_client(std::unique_ptr<rmi::transport::client_transport_iface> transport) const;

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
   * Subclasses that control transport construction call this directly
   * instead of going through `run()`.
   *
   * Does NOT catch exceptions — the caller (`run()` or the subclass override)
   * is responsible for catching and routing them to `on_error()`.
   *
   * @param transport  Heap-allocated transport to take ownership of.
   * @return Return value of `on_session()`.
   */
  virtual int run_with_transport(std::unique_ptr<rmi::transport::client_transport_iface> transport);

  /**
   * @brief Print @p prompt, then read one line of console (operator) input,
   *        blocking until a line is available.
   *
   * Backed by `line_editor_`: when stdin/stdout are an interactive tty,
   * this gets arrow-key command history and in-line cursor editing (see
   * `line_editor.hpp`); otherwise it degrades to plain
   * `std::getline(std::cin, line)`, so scripted/piped input still works.
   *
   * In `--transport=term` mode, fd 0 is backed by
   * `term::scoped_terminal_config`, which redirects the transport's real,
   * framed read fd elsewhere and feeds this class only the non-frame
   * pass-through bytes (see `client_app.cpp` and
   * `src/term/scoped_terminal_config.hpp`) — `isatty(0)` is therefore false
   * in that mode (fd 0 is a pipe, not a terminal), so `line_editor_`
   * naturally falls back to plain `std::cin` reads there, regardless of
   * transport.
   *
   * @param prompt Text to print before reading (e.g. "> ").
   * @param line   Output line, without the trailing newline.
   * @return `false` once no more input is available (EOF).
   */
  bool read_console_line(std::string_view prompt, std::string& line);

  /** @brief Per-request timeout; set from `--timeout` before `on_session()`. */
  std::chrono::milliseconds timeout_{30000};

 private:
  /** @brief Backs `read_console_line()`; persists history across calls. */
  line_editor line_editor_;
};

} // namespace bdg::bison::app
