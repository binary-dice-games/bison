// MIT License © 2025 Binary Dice Games
/**
 * @file pty_server_app.hpp
 * @brief PTY server application scaffold — thin wrapper over server_app.
 */
#pragma once

#include "src/app/server/server_app.hpp"

#include <string>

namespace bdg::bison::app {

/**
 * @brief Extensible base class for PTY bison server applications.
 *
 * `pty_server_app` is a thin wrapper around `server_app` that forces the PTY
 * transport and provides backward-compatible virtual hooks for the C ABI and
 * existing subclasses.
 *
 * Concrete applications:
 * 1. Override `register_classes()` to populate the bison class registry.
 * 2. Call `run()` from `main()` — flags are ignored; the PTY lifecycle starts
 *    immediately.
 *
 * Optional hooks mirror the older `pty_server_app` API to maintain backward
 * compatibility with `pty_c.cpp` and existing user code:
 * - `shell_command()` — shell to launch via `forkpty` (default: `"bash"`)
 * - `listen_params()` — retained for C ABI; not used internally
 * - `on_client_connected()` — called before each session starts
 * - `on_session_ended()` — called after each session ends
 * - `on_error()` — called on fatal exceptions (default: `std::cerr`)
 *
 * All session lifecycle, verbose trace, and error paths go through
 * `server_app::run_pty()` and `bridged_server`, so `on_session_created/destroyed`
 * and `--verbose` trace work automatically when subclassed.
 *
 * Linux and Windows.
 */
class pty_server_app : public server_app {
 public:
  /**
   * @brief Run the PTY server — argc/argv are ignored, PTY transport is forced.
   *
   * @return 0 on clean shell exit; 1 on error.
   */
  int run(int argc, char** argv) override;

 protected:
  /**
   * @brief Child process launched by the PTY transport via `uv_spawn()`.
   *
   * Override to specify the bison client binary to spawn (e.g.
   * `"pty_client_example"`).  The default is a bare shell, which is only
   * useful if you are piping the bison client manually.
   */
  std::string shell_command() const override { return "bash"; }

  /**
   * @brief Transport parameters (retained for C ABI compatibility).
   *
   * Not used by `pty_server_app::run()` internally; kept so that
   * `pty_c.cpp` subclasses can still override this method.
   */
  virtual bison::dynamic listen_params() const;

  /**
   * @brief Called after a client connects and before the session starts.
   *
   * Default: no-op.
   */
  virtual void on_client_connected() const {}

  /**
   * @brief Called after each session ends and all session objects are released.
   *
   * Default: no-op.
   */
  virtual void on_session_ended() const {}

 private:
  // Bridge server_app hooks to the legacy pty_server_app hook names.
  void on_pty_client_connected() const override { on_client_connected(); }
  void on_pty_session_ended()    const override { on_session_ended(); }
};

} // namespace bdg::bison::app
