// MIT License © 2025 Binary Dice Games
/**
 * @file bridge.hpp
 * @brief RMI bridge that multiplexes downstream clients into one upstream
 *        session.
 */
#pragma once

#include "src/rmi/client/client.hpp"
#include "src/rmi/server/server.hpp"
#include "src/rmi/shared/constants.hpp"
#include "src/rmi/shared/envelope.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>

namespace bdg::bison::rmi {

/**
 * @brief Multiplexing bridge between downstream RMI clients and one upstream
 *        `rmi::server`.
 *
 * The bridge inherits `rmi::server` to accept multiple downstream connections
 * and holds an `rmi::client` for the single upstream connection.
 *
 * ## Pure relay, no local object registry
 *
 * Every object-touching request (`instantiate`, `set`, `get`, `call`,
 * `clear`, `destroy`) is forwarded to the upstream server verbatim -- same
 * object ID, same payload -- via `try_handle_request()`, and the upstream
 * response is relayed straight back. The bridge never stores a local mirror
 * object for anything a downstream client instantiates: object IDs are
 * identical on both sides, and the bridge never inspects payload contents to
 * decide how to route them. This matters because not every upstream object
 * is created via a single `OP_INSTANTIATE` round trip the bridge can
 * intercept -- e.g. wish's UI template system creates a whole subtree of
 * objects as a side effect of one `OP_CALL` -- so any scheme that requires
 * recognizing "object creation" specifically would miss those. Pure
 * pass-through has no such blind spot: it doesn't need to recognize object
 * creation at all.
 *
 * Server-initiated events, being asynchronous, have no in-flight request to
 * correlate against, so they're delivered differently: every event with no
 * known owner is broadcast to every connected downstream session (see
 * `route_event()`). This is safe because each session's own event-handler
 * lookup (already unconditional on every ordinary connection) silently
 * drops anything it has no handler registered for -- an object ID is a
 * routing tag here, not a capability.
 *
 * The bridge itself still tracks nothing for cleanup purposes: every request
 * it relays is tagged with the downstream session's own ID as its object
 * *group* (`shared::envelope::group`, `context::current_group`,
 * `context::put_object()`) -- a bison-level mechanism for filing an object
 * under a caller-chosen group at creation time, regardless of whether it was
 * created directly (`OP_INSTANTIATE`) or indirectly as a side effect of
 * another op (exactly wish's UI template case above). `teardown_session()`
 * then destroys everything a session ever created with a single
 * `OP_DESTROY_GROUP` request -- otherwise those objects would leak on the
 * upstream server for the lifetime of the bridge process, since the bridge's
 * one shared upstream connection stays open across many downstream sessions
 * coming and going.
 *
 * ## Transport independence
 *
 * The downstream server transport and the upstream client transport are chosen
 * independently.  Any valid combination is supported — for example a
 * `pty_client_transport` upstream and a `socket_server_transport` downstream.
 *
 * ## Adding bridge-owned UI
 *
 * Override `on_client_connected(context&)` to inject bridge-owned objects
 * (e.g. a desktop compositor widget) into the shared upstream session whenever
 * a new downstream client connects.
 */
class bridge : public server {
 public:
  /**
   * @brief Construct with a borrowed downstream transport and an owned upstream
   *        transport.
   *
   * The downstream transport is not owned by the bridge — the caller must
   * ensure it outlives the bridge.
   *
   * @param downstream_transport    Downstream server transport (borrowed).
   * @param upstream_transport      Upstream client transport (owned).
   * @param upstream_params         Optional parameters forwarded to
   *                                `upstream_client_.connect()`.
   */
  bridge(
      transport::server_transport_iface& downstream_transport,
      std::unique_ptr<transport::client_transport_iface> upstream_transport,
      bison::dynamic upstream_params = {});

  /**
   * @brief Construct with an owned downstream transport and an owned upstream
   *        transport.
   *
   * @param downstream_transport  Downstream server transport (owned).
   * @param upstream_transport    Upstream client transport (owned).
   * @param upstream_params       Optional parameters forwarded to
   *                              `upstream_client_.connect()`.
   */
  bridge(
      std::unique_ptr<transport::server_transport_iface> downstream_transport,
      std::unique_ptr<transport::client_transport_iface> upstream_transport,
      bison::dynamic upstream_params = {});

  ~bridge();

  /**
   * @brief Connect to the upstream server, then start accepting downstream
   *        clients.
   *
   * Connects `upstream_client_` using `upstream_params_`, then calls
   * `server::listen(downstream_params)`.  Throws if the upstream connection
   * fails.
   *
   * @param downstream_params Optional parameters forwarded to
   *                          `server::listen()`.
   */
  void start(bison::dynamic downstream_params = {});

  /** @brief Stop the downstream accept loop and disconnect from upstream. */
  void stop();

 protected:
  /**
   * @brief Called after a downstream client session is registered.
   *
   * Override to inject bridge-owned UI objects into the shared upstream
   * session (e.g. a desktop compositor widget).  The base implementation is
   * a no-op.
   *
   * @param ctx Newly registered downstream session context.
   */
  virtual void on_client_connected(context& ctx) {
    (void)ctx;
  }

  /**
   * @brief Called just before a downstream client session is cleaned up.
   *
   * Override to remove any bridge-owned UI objects added in
   * `on_client_connected`.  The base implementation is a no-op.
   *
   * @param ctx Downstream session context about to be destroyed.
   */
  virtual void on_client_disconnected(context& ctx) {
    (void)ctx;
  }

  /**
   * @brief Access the upstream RMI client.
   *
   * Subclasses may call this from `on_client_connected` /
   * `on_client_disconnected` to create or modify objects on the shared
   * upstream session (e.g. desktop chrome widgets).
   *
   * Only valid after `start()` has been called and before `stop()` returns.
   */
  rmi::client& upstream() {
    return upstream_client_;
  }

  // ── server hook overrides ─────────────────────────────────────────────────

  void on_session_created(context& ctx) override;
  void on_session_destroyed(context& ctx) override;

  /**
   * Forwards every object-touching request (`instantiate`, `set`, `get`,
   * `call`, `clear`, `destroy`) to the upstream server verbatim and relays
   * the response back -- see the class-level doc comment. Returns `false`
   * for every other op (`connect`, `describe`, `disconnect`, `dictionary`,
   * `help`), letting the base `server` handle those locally as usual.
   */
  bool try_handle_request(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn) override;

 private:
  // ── Per-downstream-session relay state ───────────────────────────────────

  struct session_state {
    bison::key_t session_id;

    /** State guarded together so `emit` is never called after teardown. */
    struct emit_state {
      /**
       * Set to false by `teardown_session` while holding the write lock.
       * `route_event` checks it under the same lock before calling `emit`.
       */
      bool active{true};

      /**
       * Copy of `ctx.emit_event`.  Must only be called while holding the
       * `emit_st` lock and with `active == true`.
       */
      std::function<void(bison::key_t, bison::key_t, bison::dynamic)> emit;
    };

    /**
     * Synchronizes calls to `emit` with the `teardown_session` shutdown path.
     * `teardown_session` takes the write lock (and sets `active` to false) to
     * guarantee that any in-progress event dispatch on the upstream client
     * thread finishes before the downstream connection (`conn`) is destroyed.
     */
    bison::synchronized<emit_state> emit_st;
  };

  // ── Private helpers ───────────────────────────────────────────────────────

  /**
   * Remove the session from the sessions_ map (so route_event's broadcast
   * and further emit calls become no-ops for it), then send a single
   * OP_DESTROY_GROUP request for the session's own ID (see the class-level
   * doc comment) -- destroying every upstream object the session ever
   * created, direct or indirect, without the bridge needing to have tracked
   * any of them itself.
   */
  void teardown_session(bison::key_t session_id);

  /**
   * Route an upstream event envelope: broadcast to the bridge's own
   * upstream-client handler table (for bridge-owned UI) and to every
   * connected downstream session, relying on each recipient's own
   * event-handler lookup to discard it if irrelevant.
   */
  void route_event(const shared::envelope& env);

  // ── Members ───────────────────────────────────────────────────────────────

  /**
   * Sessions keyed by session-ID hash.  Entries are removed inside
   * teardown_session, which fires from on_session_destroyed before
   * cleanup_context.
   */
  bison::synchronized<std::unordered_map<bison::hash_t, std::shared_ptr<session_state>>> sessions_;

  /** Single upstream RMI connection (wraps a bridge_upstream_transport). */
  client upstream_client_;

  bison::dynamic upstream_params_;
  std::atomic<bool> started_{false};
};

} // namespace bdg::bison::rmi
