// MIT License © 2025 Binary Dice Games
/**
 * @file standalone_app.hpp
 * @brief In-process (transport-free) application scaffold for `rmi::standalone`.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/server/context.hpp"
#include "src/rmi/standalone/standalone.hpp"

#include <string>

namespace bdg::bison::app {

/**
 * @brief Extensible base class for bison RMI standalone (in-process) applications.
 *
 * `rmi::standalone` combines client and server logic without a transport, so
 * unlike `client_app`/`server_app` there is no `--transport` flag to parse --
 * `standalone_app` only handles the `--debugger` flag and the
 * connect/session/disconnect lifecycle. Concrete applications override
 * `register_classes()` (mirrors `server_app`) to populate the class registry
 * and `on_session()` (mirrors `client_app`) to drive their domain logic
 * through the in-process session.
 *
 * Typical lifecycle (inside `run()`):
 * 1. Parse flags.
 * 2. `register_classes()`.
 * 3. Construct an `rmi::standalone` and call `connect()`.
 * 4. `on_session(sa)` -- application logic (pure virtual).
 * 5. `sa.disconnect()`.
 * 6. On exception: `on_error(msg)`, return 1.
 */
class standalone_app {
 public:
  virtual ~standalone_app() = default;

  /**
   * @brief Parse flags, register classes, run the session, disconnect.
   *
   * @param argc  Argument count from `main`.
   * @param argv  Argument vector from `main`.
   * @return Value returned by `on_session()`, or 1 on error.
   */
  virtual int run(int argc, char** argv);

  /**
   * @brief Called after the standalone session context is created.
   *
   * Fires the first time `rmi::standalone::connect()` is called, mirroring
   * `server_app::on_session_created()`. Default: no-op.
   *
   * Public (not protected) so the internal `rmi::standalone` subclass that
   * `run()` constructs -- which does not inherit from `standalone_app` -- can
   * forward to it, mirroring `server_app::on_session_created()`.
   *
   * @param ctx Newly created session context.
   */
  virtual void on_session_created(rmi::context& ctx) const {
    (void)ctx;
  }

  /**
   * @brief Called just before the standalone session context is destroyed.
   *
   * Fires the first time `rmi::standalone::disconnect()` is called, mirroring
   * `server_app::on_session_destroyed()`. Default: no-op.
   *
   * @param ctx Session context about to be destroyed.
   */
  virtual void on_session_destroyed(rmi::context& ctx) const {
    (void)ctx;
  }

 protected:
  /**
   * @brief Register domain classes in the bison class registry.
   *
   * Called once before the standalone session is created. Use
   * `bison::dynamic::addClass()` to register prototype objects.
   */
  virtual void register_classes() = 0;

  /**
   * @brief Main application logic for the in-process session.
   *
   * Called after `register_classes()` and after the session is connected.
   * The return value becomes the return value of `run()`.
   *
   * @param sa Connected standalone session.
   * @return Exit code.
   */
  virtual int on_session(rmi::standalone& sa) = 0;

  /**
   * @brief Called when a session exception is caught.
   *
   * Default: writes to `std::cerr`.
   *
   * @param msg Human-readable error description.
   */
  virtual void on_error(const std::string& msg) const;
};

} // namespace bdg::bison::app
