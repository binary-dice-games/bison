// MIT License © 2025 Binary Dice Games
/**
 * @file bridge_app.hpp
 * @brief Generic multi-transport bridge application scaffold.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/server/context.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <memory>
#include <string>

namespace bdg::bison::rmi {
class bridge;
} // namespace bdg::bison::rmi

namespace bdg::bison::term {
class terminal;
} // namespace bdg::bison::term

namespace bdg::bison::app {

/**
 * @brief Extensible base class for bison RMI bridge applications.
 *
 * Handles command-line transport selection for both the downstream (client-
 * accepting) and upstream (single relayed connection) sides of an
 * `rmi::bridge`, plus the start/stop lifecycle. Concrete bridge applications
 * override `bridge_description()` and the `on_client_connected`/
 * `on_client_disconnected` hooks to inject bridge-owned state, then call
 * `run()` from `main()`.
 *
 * Every downstream-side flag is `downstream_`-prefixed (mirroring the
 * existing `upstream_` prefix on the upstream side) so the two transports a
 * bridge has active at once -- and their host/port/name/transport-kind --
 * are never confused with each other, e.g. in `--help` output.
 *
 * Supported gflags CLI flags:
 *   - `--downstream_transport T` downstream transport: `tcp` (default),
 *                                `pipe`, or `term`. Same semantics as
 *                                `server_app`'s `--transport`; a bridge's
 *                                downstream side behaves exactly like a
 *                                server's, but uses its own flag name so it
 *                                is never confused with `--upstream_transport`.
 *   - `--downstream_host HOST --downstream_port PORT` downstream TCP bind
 *                                address (downstream_transport=tcp)
 *   - `--downstream_name PATH`   downstream named-pipe / Unix-socket path
 *                                (downstream_transport=pipe)
 *   - `--cmd`                    command to spawn for the downstream terminal
 *                                (downstream_transport=term)
 *   - `--upstream_transport T`   upstream transport: `tcp` (default), `pipe`,
 *                                or `term`.
 *   - `--upstream_host HOST --upstream_port PORT` upstream TCP address
 *                                (upstream_transport=tcp)
 *   - `--upstream_name PATH`     upstream named-pipe / Unix-socket path
 *                                (upstream_transport=pipe)
 *   - `--timeout MS`             upstream per-request timeout
 *   - `--verbose`                print request/response trace lines for the
 *                                downstream side (same as `server_app`)
 *
 * Typical lifecycle:
 * 1. Build the downstream transport and the upstream transport from flags.
 * 2. `run_with_transport(downstream, upstream)` — connects upstream, then
 *    starts accepting downstream connections.
 * 3. Call `on_listening()`.
 * 4. Serve until Enter is pressed (socket / named-pipe downstream) or the
 *    spawned shell exits (`--downstream_transport=term`).
 * 5. Stop the bridge and disconnect from upstream.
 */
class bridge_app {
 public:
  virtual ~bridge_app() = default;

  /**
   * @brief Parse flags, start the bridge, and block until done.
   *
   * @param argc  Argument count from `main`.
   * @param argv  Argument vector from `main`.
   * @return 0 on clean shutdown, 1 on error.
   */
  virtual int run(int argc, char** argv);

  /**
   * @brief Called after the bridge starts listening for downstream connections.
   *
   * Default: prints a ready message describing both the downstream and
   * upstream endpoints to stdout.
   */
  virtual void on_listening() const;

  /**
   * @brief Called on fatal errors before `run()` returns 1.
   *
   * @param msg  Human-readable error description.
   */
  virtual void on_error(const std::string& msg) const;

  /**
   * @brief Return an optional preamble for `OP_HELP` responses.
   *
   * Mirrors `server_app::server_description()`. Default: returns an empty
   * string (no preamble).
   */
  virtual std::string bridge_description() const {
    return {};
  }

  /**
   * @brief Called after a downstream client session is registered.
   *
   * Forwards `rmi::bridge::on_client_connected()`. Default: no-op.
   *
   * Public (not protected) so the internal `rmi::bridge` subclass that
   * `run_with_transport()` constructs -- which does not inherit from
   * `bridge_app` -- can forward to it, mirroring
   * `server_app::on_session_created()`.
   *
   * @param ctx Newly registered downstream session context.
   */
  virtual void on_client_connected(rmi::context& ctx) const {
    (void)ctx;
  }

  /**
   * @brief Called just before a downstream client session is cleaned up.
   *
   * Forwards `rmi::bridge::on_client_disconnected()`. Default: no-op.
   *
   * @param ctx Downstream session context about to be destroyed.
   */
  virtual void on_client_disconnected(rmi::context& ctx) const {
    (void)ctx;
  }

 protected:
  /**
   * @brief Populate upstream connection parameters before the bridge connects.
   *
   * Default: sets `timeout_ms` to the value of `FLAGS_timeout` (or 30 000 if
   * not parsed yet).
   *
   * @param params In/out parameter map.
   */
  virtual void on_upstream_connect_params(bison::dynamic& params) const;

  /**
   * @brief Construct the bridge bound to the given transports.
   *
   * Override to return a subclass of `rmi::bridge` (e.g. one that injects
   * bridge-owned UI via its own `on_client_connected`/`on_client_disconnected`
   * overrides, using `upstream()` directly) instead of relying on this
   * class's `on_client_connected`/`on_client_disconnected` hook forwarding.
   * Default: an internal bridge that forwards `on_client_connected`/
   * `on_client_disconnected`/`bridge_description` to this app's hooks of the
   * same name.
   *
   * @param downstream          Downstream server transport (borrowed).
   * @param upstream_transport  Upstream client transport (owned).
   * @param upstream_params     Parameters forwarded to `upstream_client_.connect()`.
   * @return Newly constructed bridge, not yet started.
   */
  virtual std::unique_ptr<rmi::bridge> make_bridge(
      rmi::transport::server_transport_iface& downstream_transport,
      std::unique_ptr<rmi::transport::client_transport_iface> upstream_transport,
      bison::dynamic upstream_params);

  /**
   * @brief Construct the bridge, start it, block until shutdown, then stop.
   *
   * Subclasses that control transport construction call this directly
   * instead of going through `run()`.
   *
   * Does NOT catch exceptions -- the caller (`run()` or the subclass override)
   * is responsible for catching and routing them to `on_error()`.
   *
   * @param downstream_transport  Downstream server transport (borrowed).
   * @param upstream_transport    Upstream client transport (owned).
   * @return 0 on clean shutdown, non-zero on error.
   */
  virtual int run_with_transport(
      rmi::transport::server_transport_iface& downstream_transport,
      std::unique_ptr<rmi::transport::client_transport_iface> upstream_transport);

  /**
   * @brief Block until the bridge should stop.
   *
   * Default: waits on `active_term_` (the spawned terminal exiting) when
   * `--downstream_transport=term` selected it, otherwise blocks on
   * `std::getline(std::cin, line)`. Override to add another shutdown
   * trigger (e.g. a UI "Quit" action); an override that must preserve the
   * term-transport wait should check `active_term_` and delegate to this
   * base implementation when it is set.
   */
  virtual void wait_for_shutdown();

  /**
   * @brief Non-owning pointer to the spawned terminal when
   *        `--downstream_transport=term` selected it, set by `run()` for
   *        the duration of the term-transport `run_with_transport()` call;
   *        null otherwise. Read by the default `wait_for_shutdown()`.
   */
  term::terminal* active_term_ = nullptr;
};

} // namespace bdg::bison::app
