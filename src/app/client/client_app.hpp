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
#include <queue>
#include <string>
#include <string_view>

namespace bdg::bison::rmi::transport {
class stdio_client_transport;
} // namespace bdg::bison::rmi::transport

namespace bdg::bison::app {

/**
 * @brief Extensible base class for bison RMI client applications.
 *
 * Handles command-line transport selection and the connect/session/disconnect
 * lifecycle.  Concrete applications override `on_session()` to drive their
 * domain logic through the connected RMI client.
 *
 * Transport is chosen by gflags CLI flags (all optional):
 *   - `--host HOST --port PORT` — TCP socket (default: `127.0.0.1:7070`)
 *   - `--pipe PATH`             — named-pipe / Unix-socket path
 *   - `--pty`                   — wrap this process's own inherited `fd 0`
 *                                 (read) / `fd 1` (write) in the `BISON:`
 *                                 line framing instead of opening a
 *                                 socket/pipe. No pty is spawned here — the
 *                                 client never forks a terminal; this is for
 *                                 running as a plain child process whose
 *                                 stdio is already connected to a peer that
 *                                 speaks the framing (typically because it
 *                                 was launched inside a server-spawned
 *                                 `--pty` terminal, see
 *                                 `src/app/server/server_app.hpp` and
 *                                 `src/rmi/transport/stdio_transport.hpp`).
 *                                 Also puts fd 0 in raw mode for the session
 *                                 (`pty::raw_mode_guard`) and requires
 *                                 subclasses to read operator input via
 *                                 `read_console_line()` rather than
 *                                 `std::cin` directly — see that method's
 *                                 doc comment.
 *   - `--timeout MS`            — per-request timeout stored in `timeout_`
 *
 * `--pty` takes precedence over `--pipe`, which takes precedence over
 * `--host`/`--port`.
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
  int run_with_transport(std::unique_ptr<rmi::transport::client_transport_iface> transport);

  /**
   * @brief Read one line of console (operator) input, blocking until a line
   *        is available.
   *
   * In `--host`/`--port` and `--pipe` modes this reads `std::cin` directly,
   * since fd 0 there is the operator's own terminal, untouched by the RMI
   * transport. In `--pty` mode, fd 0 is instead the wire the transport reads
   * `BISON:` frames from in a background thread — `std::cin` would race
   * that reader and fail immediately (the transport puts the fd in
   * non-blocking mode) — so this instead drains lines assembled from the
   * non-frame bytes the transport hands to its passthrough callback (see
   * `client_app.cpp` and `src/pty/DESIGN.md`).
   *
   * @param line Output line, without the trailing newline.
   * @return `false` once no more input is available (EOF on `std::cin`, or
   *         the pty stream closed).
   */
  bool read_console_line(std::string& line);

  /** @brief Per-request timeout; set from `--timeout` before `on_session()`. */
  std::chrono::milliseconds timeout_{30000};

 private:
  /** @brief State fed by the `--pty` passthrough callback; see `read_console_line()`. */
  struct console_queue_state {
    std::string partial;
    std::queue<std::string> lines;
    bool closed{false};
  };

  /**
   * @brief Splits passthrough bytes into lines for `read_console_line()`,
   *        applying basic backspace handling (`0x7f`/`0x08`) so the line
   *        buffer matches what's on screen. An empty chunk signals stream
   *        closure. Also locally echoes each byte via `echo_transport_`
   *        (see that member's doc comment for why this is needed at all).
   */
  void feed_console_passthrough(std::string_view chunk);

  bool console_via_passthrough_{false};
  bison::synchronized<console_queue_state> console_queue_;

  /**
   * @brief Non-owning pointer to the `--pty` transport, set right after
   *        construction and valid for the rest of the process's lifetime;
   *        used only to locally echo the operator's keystrokes.
   *
   * `raw_mode_guard` disables the pty slave's kernel `ECHO` (see its doc
   * comment for why), which as a side effect means the operator's own
   * typed characters are never echoed anywhere — nothing appears on screen
   * while typing, even though the REPL is receiving them correctly. This
   * reimplements just that echo in software, in `feed_console_passthrough`.
   * Null outside `--pty` mode.
   */
  rmi::transport::stdio_client_transport* echo_transport_{nullptr};
};

} // namespace bdg::bison::app
