// MIT License © 2025 Binary Dice Games
/**
 * @file pty_server_app.hpp
 * @brief Multi-session PTY server application scaffold.
 */
#pragma once

#if defined(__linux__)

#include "src/bison/bison.hpp"

#include <string>

namespace bdg::bison::app {

/**
 * @brief Extensible base class for multi-session PTY server applications.
 *
 * Concrete applications subclass `pty_server_app`, override
 * `register_classes()` to expose their domain objects in the bison class
 * registry, and call `run()` from `main()`.  Optional hooks allow
 * customisation of the shell, transport parameters, and per-session
 * lifecycle events.
 *
 * The base class manages:
 * - Calling `register_classes()` once before the session loop.
 * - Starting `pty_server_transport` once (the shell subprocess runs for the
 *   lifetime of the process).
 * - Looping: wait for a client HELLO → create `rmi::server` for the session
 *   → call `on_client_connected()` → wait for disconnect → destroy the server
 *   (releasing all session objects) → call `on_session_ended()` →
 *   `restart_session()` → repeat.
 * - Stopping cleanly when the shell subprocess exits.
 *
 * Linux only.
 */
class pty_server_app {
 public:
  virtual ~pty_server_app() = default;

  /**
   * @brief Run the multi-session server loop.
   *
   * Blocks until the shell subprocess exits or a fatal error occurs.
   *
   * @return 0 on clean shell exit; 1 on error.
   */
  int run(int argc, char** argv);

 protected:
  /**
   * @brief Register domain classes in the bison global class registry.
   *
   * Called once before the session loop.  Implementations should call
   * `bison::register_class<T>()` (or equivalent) for each exposed type.
   */
  virtual void register_classes() = 0;

  /**
   * @brief Shell command passed to `forkpty` (default: `"bash"`).
   *
   * Override to use a different shell (e.g. `"sh"`, `"zsh"`, `"fish"`).
   */
  virtual std::string shell_command() const;

  /**
   * @brief Transport parameters applied when `pty_server_transport::start()`
   *        is called (default: `mode=dcs`).
   *
   * The PTY transport forces DCS mode regardless; these params are passed
   * through for any future extension points.
   */
  virtual bison::dynamic listen_params() const;

  /**
   * @brief Called once after a client connects and the `rmi::server` is ready.
   */
  virtual void on_client_connected() const;

  /**
   * @brief Called after each session ends and the `rmi::server` is destroyed.
   */
  virtual void on_session_ended() const;

  /**
   * @brief Called when a transport-level exception is caught.
   * @param msg Human-readable error description.
   */
  virtual void on_error(const std::string& msg) const;
};

} // namespace bdg::bison::app

#endif // defined(__linux__)
