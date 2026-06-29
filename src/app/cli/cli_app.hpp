// MIT License © 2025 Binary Dice Games
/**
 * @file cli_app.hpp
 * @brief Interactive REPL client for bison RMI servers.
 */
#pragma once

#include "src/app/client/client_app.hpp"

namespace bdg::bison::app {

/**
 * @brief Interactive command-line REPL for bison RMI servers.
 *
 * `cli_app` is a thin subclass of `client_app` that provides a
 * scripting-style REPL as its `on_session()` implementation.  Transport
 * selection, flag parsing, and the connect/disconnect lifecycle are all
 * handled by `client_app`.
 *
 * Default `on_session()` runs the built-in REPL:
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
 * Override `on_session()` to replace the REPL with custom behaviour (for
 * example, a scripted test or a full-screen terminal UI).
 *
 * Transport flags (from `client_app`):
 *   - `--host HOST --port PORT` — TCP socket (default: `127.0.0.1:7070`)
 *   - `--pipe PATH`             — named-pipe / Unix-socket path
 *   - `--timeout MS`            — per-request timeout (default: 30 000 ms)
 */
class cli_app : public client_app {
 protected:
  /**
   * @brief Run the interactive scripting REPL.
   *
   * Default implementation.  Override to replace with custom session logic.
   *
   * @param c Connected RMI client.
   * @return 0 on clean exit (user typed `exit`/`quit` or EOF).
   */
  int on_session(rmi::client& c) override;

  /**
   * @brief Called when a transport or session exception is caught.
   *
   * Default: writes to `std::cerr` with a `[cli_app]` prefix.
   */
  void on_error(const std::string& msg) const override;
};

} // namespace bdg::bison::app
