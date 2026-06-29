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
#include <utility>

namespace bdg::bison::rmi {

/**
 * @brief Multiplexing bridge between downstream RMI clients and one upstream
 *        `rmi::server`.
 *
 * The bridge inherits `rmi::server` to accept multiple downstream connections
 * and holds an `rmi::client` for the single upstream connection.  Every object
 * instantiated by a downstream client is transparently forwarded to the
 * upstream server; all method calls, field sets/gets, clears, and destructions
 * are relayed through an in-memory proxy object.
 *
 * ## Transport independence
 *
 * The downstream server transport and the upstream client transport are chosen
 * independently.  Any valid combination is supported — for example a
 * `pty_client_transport` upstream and a `socket_server_transport` downstream.
 *
 * ## Namespace isolation
 *
 * Each downstream session is assigned a unique `ns_prefix` key.  The bridge
 * enforces that downstream clients cannot access each other's upstream objects:
 * each session owns a private object-ID translation table, and the upstream
 * server never receives downstream-side IDs directly.
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
   * @param downstream        Downstream server transport (borrowed).
   * @param upstream_transport Upstream client transport (owned).
   * @param upstream_params   Optional parameters forwarded to
   *                          `upstream_client_.connect()`.
   */
  bridge(
      transport::server_transport_iface& downstream,
      std::unique_ptr<transport::client_transport_iface> upstream_transport,
      bison::dynamic upstream_params = {});

  /**
   * @brief Construct with an owned downstream transport and an owned upstream
   *        transport.
   *
   * @param downstream        Downstream server transport (owned).
   * @param upstream_transport Upstream client transport (owned).
   * @param upstream_params   Optional parameters forwarded to
   *                          `upstream_client_.connect()`.
   */
  bridge(
      std::unique_ptr<transport::server_transport_iface> downstream,
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
   * Always returns true: the bridge accepts any class instantiation and
   * forwards it to the upstream server, bypassing the local class registry.
   */
  bool on_check_class(context& ctx, bison::key_t ns, bison::key_t klass) override;

  /** Intercepts object instantiation to create upstream proxy objects. */
  bison::dynamic_ptr on_create_object(context& ctx, bison::key_t ns, bison::key_t klass) override;

  /**
   * Saves the current request ID for use in `on_create_object` and handles
   * OP_CLEAR forwarding to upstream before `handle_clear` resets the local
   * proxy.
   */
  void on_request_trace(context& ctx, const shared::envelope& env) override;

  /**
   * Finalizes the upstream ↔ local object ID mapping after OP_INSTANTIATE,
   * and re-installs the forwarding proxy after OP_CLEAR.
   */
  void on_response_trace(
      context& ctx,
      const shared::envelope& request_env,
      bison::key_t op,
      bool is_error,
      bison::key_t error_code,
      const bison::dynamic& response_payload) override;

 private:
  // ── Per-downstream-session relay state ───────────────────────────────────

  struct session_state {
    bison::key_t session_id;

    /** Unique key identifying this client's logical namespace. */
    bison::key_t ns_prefix;

    /**
     * Mutex that serialises calls to `emit` and the `teardown_session`
     * shutdown path.  `teardown_session` acquires it (while `emit_active` is
     * set to false) to guarantee that any in-progress event dispatch on the
     * upstream client thread finishes before the downstream connection (`conn`)
     * is destroyed.
     */
    std::mutex emit_mtx;

    /**
     * Set to false by `teardown_session` while holding `emit_mtx`.
     * `route_event` checks it under `emit_mtx` before calling `emit`.
     */
    bool emit_active{true};

    /**
     * Copy of `ctx.emit_event`.  Must only be called while holding
     * `emit_mtx` and with `emit_active == true`.
     */
    std::function<void(bison::key_t, bison::key_t, bison::dynamic)> emit;

    /** downstream local_oid → upstream_oid */
    bison::synchronized<std::unordered_map<bison::hash_t, bison::key_t>> local_to_upstream;

    /** upstream_oid → downstream local_oid */
    bison::synchronized<std::unordered_map<bison::hash_t, bison::key_t>> upstream_to_local;
  };

  // ── Temporary state during OP_INSTANTIATE dispatch ───────────────────────

  struct pending_relay {
    bison::key_t upstream_oid;
    bison::key_t session_id;
    /** Shared slot filled in on_response_trace; HOOK_DESTRUCT reads it. */
    std::shared_ptr<bison::key_t> local_oid_slot;
  };

  // ── Thread-local state used across on_request_trace / on_create_object /
  //    on_response_trace ──────────────────────────────────────────────────

  struct pending_clear_state {
    bool active{false};
    bison::key_t local_oid;
    bison::key_t upstream_oid;
    bison::key_t session_id;
  };

  // ── Private helpers ───────────────────────────────────────────────────────

  std::shared_ptr<session_state> find_session(bison::key_t id) const;

  /**
   * Remove the session from the sessions_ map (so event callbacks become
   * no-ops) and unregister all upstream event handlers for that session's
   * objects.  Actual upstream object destruction is handled by the proxy
   * objects' HOOK_DESTRUCT hooks, called by cleanup_context after this.
   */
  void teardown_session(bison::key_t session_id);

  /**
   * Construct a `bison::dynamic` proxy that forwards all operations to the
   * upstream object identified by @p upstream_oid.
   *
   * @param upstream_oid   Upstream server object identifier.
   * @param ws             Weak reference to the owning session state.
   * @param local_oid_slot Shared slot populated by on_response_trace when the
   *                       local object ID is known; HOOK_DESTRUCT reads it to
   *                       update translation tables.
   */
  bison::dynamic_ptr make_proxy_obj(
      bison::key_t upstream_oid,
      std::weak_ptr<session_state> ws,
      std::shared_ptr<bison::key_t> local_oid_slot);

  /** Route an upstream event envelope to the correct downstream session. */
  void route_event(const shared::envelope& env);

  // ── Members ───────────────────────────────────────────────────────────────

  /**
   * Sessions keyed by session-ID hash.  Entries are removed inside
   * teardown_session, which fires from on_session_destroyed before
   * cleanup_context.  HOOK_DESTRUCT lambdas hold a weak_ptr so they become
   * no-ops once the entry is removed.
   */
  bison::synchronized<std::unordered_map<bison::hash_t, std::shared_ptr<session_state>>> sessions_;

  /**
   * Global reverse lookup: upstream_oid_hash → (session_id, local_oid).
   * Used by route_event to find which downstream session owns a given upstream
   * object in O(1).  Updated in on_response_trace and HOOK_DESTRUCT.
   */
  bison::synchronized<std::unordered_map<bison::hash_t, std::pair<bison::key_t, bison::key_t>>> // session_id, local_oid
      upstream_to_session_;

  /**
   * Temporary relays keyed by request_id hash.  Written by on_create_object
   * and consumed (erased) by on_response_trace for OP_INSTANTIATE.  Both
   * accesses happen on the same server worker thread, so the synchronized<>
   * lock is never recursively nested.
   */
  bison::synchronized<std::unordered_map<bison::hash_t, pending_relay>> pending_relays_;

  /** Single upstream RMI connection (wraps a bridge_upstream_transport). */
  client upstream_client_;

  bison::dynamic upstream_params_;
  std::atomic<uint32_t> ns_counter_{0};
  std::atomic<bool> started_{false};

  /**
   * Thread-local: the request_id of the envelope currently being dispatched.
   * Set in on_request_trace; read in on_create_object to key pending_relays_.
   * Safe because each server worker thread handles exactly one request at a
   * time.
   */
  static thread_local bison::key_t current_request_id_;

  /**
   * Thread-local: state for the OP_CLEAR currently being dispatched.
   * Set in on_request_trace before handle_clear runs; consumed in
   * on_response_trace after handle_clear has reset the local proxy.
   */
  static thread_local pending_clear_state pending_clear_;
};

} // namespace bdg::bison::rmi
