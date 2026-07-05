// MIT License © 2025 Binary Dice Games
/**
 * @file standalone.hpp
 * @brief Standalone in-process RMI mode combining client and server logic
 *        without a transport layer.
 *
 * Include this header (or `src/rmi/rmi.hpp`) to use `standalone` as a
 * zero-serialization drop-in alternative to the transport-backed client.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"
#include "src/rmi/server/context.hpp"

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <queue>
#include <thread>
#include <unordered_map>

namespace bdg::bison::rmi {

/**
 * @brief In-process RMI runtime that combines client and server logic without
 *        serialization or transport overhead.
 *
 * Unlike the transport-backed `client`/`server` pair, `standalone` operates
 * directly on live `bison::dynamic` object references held in an internal
 * server `context`.  There is no wire protocol and no serialization; every
 * operation is queued to a background worker thread and returns a pending
 * future that resolves once the worker completes the work, mirroring the
 * asynchronous semantics of the transport-backed `client`.
 *
 * The public interface intentionally mirrors `client` so that code written
 * against either class can share helper logic.  `standalone` inherits
 * `proxy_backend`, which lets `proxy::dynamic` instances created by
 * `instantiate()` dispatch their operations back here transparently—the same
 * `proxy::dynamic` type is used by both `client` and `standalone`.
 *
 * ## Typical usage
 *
 * ```cpp
 * // Register classes once at startup.
 * bison::dynamic::registerClass<MyClass>("MyClass");
 *
 * bdg::bison::rmi::standalone sa;
 * auto prx = sa.instantiate("MyClass"_key).get();
 * prx.set({...}).get();
 * auto result = prx.call("myMethod"_key, {}).get();
 * sa.destroy(std::move(prx));
 * ```
 *
 * ## Thread safety
 *
 * `standalone` is thread-safe for concurrent proxy operations.  All
 * operations are serialized through a single background worker thread.
 * The session context (`session_context()`) is wrapped in
 * `bison::synchronized<context>` and may be read or written safely from
 * external threads via `rlock()`/`wlock()`.
 *
 * @note Event handlers are invoked on the worker thread.  Do not block the
 *       worker from within an event handler (e.g. do not call `.get()` on a
 *       future returned by a proxy operation inside an event handler).
 */
class standalone : public proxy_backend {
 public:
  standalone();
  ~standalone() override;

  standalone(const standalone&) = delete;
  standalone& operator=(const standalone&) = delete;
  standalone(standalone&&) = delete;
  standalone& operator=(standalone&&) = delete;

  // ── Client-compatible interface ──────────────────────────────────────────

  /**
   * @brief Compatibility shim for code that calls `connect` generically.
   *
   * The worker thread starts in the constructor, so this never opens
   * anything -- but the *first* call fires `on_session_created()` (with
   * `ctx_`'s lock already released, so the hook may safely call back into
   * `instantiate()`/other operations).  Subsequent calls are no-ops.
   *
   * @param params Ignored.
   */
  void connect(bison::dynamic params = bison::dynamic{});

  /**
   * @brief Query registered class metadata from the local class registry.
   *
   * Replicates the server-side `describe` handler but without transport or
   * serialization.
   *
   * @param klass Class key to query.  Pass `0` (the default) to retrieve
   *              descriptors for all registered classes.
   * @return Future already resolved with the description payload.
   */
  std::future<bison::dynamic> describe(bison::key_t ns = 0U, bison::key_t klass = 0U);

  /**
   * @brief Instantiate an object from the local class registry.
   *
   * Looks up @p klass in the global `bison::dynamic` registry, creates an
   * instance, invokes the optional `__construct` hook with @p params, stores
   * the object in the internal session context, and returns a proxy that owns
   * the object ID.
   *
   * @param ns    Namespace key; `0U` selects the global namespace.
   * @param klass Class key to instantiate.
   * @param params Optional constructor parameters forwarded to `__construct`.
   * @return Future already resolved with a `proxy::dynamic` owning the new
   *         object.
   * @throws std::runtime_error if the class is not registered.
   */
  std::future<proxy::dynamic>
  instantiate(bison::key_t ns, bison::key_t klass, bison::dynamic params = bison::dynamic{});

  /**
   * @brief Destroy an object owned by @p proxy.
   *
   * Invokes the optional `__destruct` hook, removes the object from the
   * internal session context, and invalidates @p proxy.
   *
   * @param proxy Owning proxy to invalidate and destroy.
   */
  void destroy(proxy::dynamic&& proxy);

  /**
   * @brief Compatibility shim for code that calls `disconnect` generically.
   *
   * There is no transport to close in standalone mode, but the *first* call
   * fires `on_session_destroyed()` before stopping the worker thread.
   * Subsequent calls are no-ops.
   */
  void disconnect();

  /**
   * @brief Return the hash→display-name dictionary for all registered classes.
   *
   * Mirrors `server::handle_dictionary` without transport.
   *
   * @return Future already resolved with a flat `bison::dynamic` mapping each
   *         `DisplayName`-annotated item's hash to its display-name string.
   */
  std::future<bison::dynamic> get_dictionary();

  /**
   * @brief Return human-readable help text describing the registered classes.
   *
   * Mirrors `server::handle_help` without transport.  The auto-generated
   * listing includes all classes with a `DisplayName` attribute.
   *
   * @return Future already resolved with a `bison::dynamic` containing
   *         `FIELD_DESCRIPTION` → help text string.
   */
  std::future<bison::dynamic> get_help();

  /**
   * @brief Dispatch a protocol operation asynchronously on the worker thread.
   *
   * The operation is queued and the calling thread returns immediately with a
   * pending future.  The worker thread executes the operation and resolves the
   * future with the result (or an exception on failure).
   *
   * @param op        Operation token (`OP_CLEAR`, `OP_SET`, `OP_GET`,
   *                  `OP_CALL`, or `OP_DESTROY`).
   * @param object_id Target object identifier issued by `instantiate`.
   * @param payload   Operation-specific payload (consumed by move).
   * @param oneway    When true, an empty result is returned without executing
   *                  any return-value logic.  For `OP_CALL` only.
   * @return Pending future resolved on the worker thread with the operation
   *         result.
   */
  std::future<bison::dynamic> send_request(bison::key_t op, bison::key_t object_id, bison::dynamic payload, bool oneway)
      override;

  /**
   * @brief Register an event handler for a named event on an object.
   *
   * Handlers are invoked on the worker thread from within `send_request`
   * when the object's code calls `ctx_.emit_event`.
   *
   * @param object_id Object that emits the event.
   * @param name      Event name token.
   * @param handler   Callback invoked with the event payload.
   */
  void register_event_handler(bison::key_t object_id, bison::key_t name, std::function<void(bison::dynamic)> handler)
      override;

  /**
   * @brief Remove all event handlers registered for @p object_id.
   *
   * @param object_id Object whose handlers should be cleared.
   */
  void unregister_object_events(bison::key_t object_id);

  /**
   * @brief Return a reference to the synchronized session context.
   *
   * The returned `bison::synchronized<context>` may be read or written safely
   * from any thread via `rlock()` and `wlock()`.  The worker thread holds the
   * write lock while executing each operation.
   */
  bison::synchronized<context>& session_context() {
    return ctx_;
  }

  /** @brief Const overload of `session_context()`. */
  const bison::synchronized<context>& session_context() const {
    return ctx_;
  }

 protected:
  // ── Extensibility hooks (mirror bison::rmi::server's hooks of the same
  //    name/signature) ───────────────────────────────────────────────────

  /**
   * @brief Called once, the first time `connect()` is invoked.
   *
   * Fires on the calling thread with `ctx_`'s lock already released, so the
   * override is free to call back into `instantiate()`/other operations
   * (which run on the worker thread) without deadlocking.  Override to
   * attach per-session state.
   *
   * @param ctx The standalone session's context.
   */
  virtual void on_session_created(context& ctx) {
    (void)ctx;
  }

  /**
   * @brief Called once, the first time `disconnect()` is invoked.
   *
   * Fires on the calling thread with `ctx_`'s lock already released.
   *
   * @note Not called when the destructor triggers worker shutdown as a
   *       safety net (i.e. `disconnect()` was never called explicitly) —
   *       virtual dispatch is unavailable once the derived part of the
   *       object has been torn down, mirroring `client::on_disconnect()`'s
   *       documented caveat.  Call `disconnect()` explicitly before
   *       destruction if this hook must fire.
   *
   * @param ctx The standalone session's context.
   */
  virtual void on_session_destroyed(context& ctx) {
    (void)ctx;
  }

  /**
   * @brief Factory hook called when a client instantiates a new object.
   *
   * The default implementation is `bison::dynamic::create_instance`, which
   * (unlike `bison::dynamic::instantiate`) respects a factory registered via
   * `dynamic::addClass(..., factory)` -- override to return a session-aware
   * subclass instead (e.g. a handler holding a reference to per-session
   * application state).
   *
   * Called from the worker thread with `ctx_`'s write lock held for the
   * duration of the call -- do not perform blocking operations that require
   * the worker thread (e.g. `.get()` on a future from `instantiate()` or
   * `send_request()`), since the worker thread is currently executing this
   * call and cannot service nested requests.
   *
   * @param ctx   The standalone session's context.
   * @param ns    Namespace key of the requested class.
   * @param klass Class key of the requested type.
   * @return Heap-allocated `dynamic` (or subclass) for the new object.
   */
  virtual bison::dynamic_ptr on_create_object(context& ctx, bison::key_t ns, bison::key_t klass) {
    (void)ctx;
    return bison::dynamic::create_instance(ns, klass);
  }

  /**
   * @brief Called on the worker thread immediately before dispatching each
   *        queued operation (`describe`, `instantiate`, `clear`, `set`,
   *        `get`, `call`, `destroy`, `get_dictionary`, `get_help`).
   *
   * Override to acquire per-session resources (e.g. a render mutex) before
   * the operation runs.  Paired with `on_after_dispatch`.
   *
   * @param ctx The standalone session's context.
   */
  virtual void on_before_dispatch(context& ctx) {
    (void)ctx;
  }

  /**
   * @brief Called on the worker thread immediately after each queued
   *        operation completes, whether it succeeded or threw.
   *
   * Guaranteed to be called exactly once per `on_before_dispatch` call.
   * Must not throw -- override to release resources acquired in
   * `on_before_dispatch`.
   *
   * @param ctx The standalone session's context.
   */
  virtual void on_after_dispatch(context& ctx) noexcept {
    (void)ctx;
  }

 private:
  // ── Task queue ────────────────────────────────────────────────────────────

  /**
   * @brief Unit of work dispatched to the worker thread.
   */
  struct task_item {
    std::function<bison::dynamic()> work;
    std::promise<bison::dynamic> promise;
  };

  /**
   * @brief Enqueue @p work and return a future for its result.
   *
   * Returns an exception future immediately if the worker is not running.
   */
  std::future<bison::dynamic> enqueue(std::function<bison::dynamic()> work);

  /**
   * @brief Worker loop: dequeue and execute tasks until stopped.
   *
   * Remaining tasks in the queue are drained (executed, not failed) when
   * `running_` transitions to false.
   */
  void worker_loop();

  /**
   * @brief Signal the worker to stop, drain the remaining queue, and join
   *        the worker thread.
   *
   * Safe to call multiple times; subsequent calls are no-ops.
   */
  void stop_worker();

  // ── Operation handlers (mirror server-side logic without transport) ───────

  bison::dynamic handle_describe(bison::key_t ns, bison::key_t klass);
  bison::dynamic handle_dictionary();
  bison::dynamic handle_help();
  bison::dynamic handle_instantiate(bison::key_t ns, bison::key_t klass, bison::dynamic params);
  bison::dynamic handle_clear(bison::key_t object_id);
  bison::dynamic handle_set(bison::key_t object_id, bison::dynamic payload);
  bison::dynamic handle_get(bison::key_t object_id, bison::dynamic projection);
  bison::dynamic handle_call(bison::key_t object_id, bison::dynamic payload, bool oneway);
  bison::dynamic handle_destroy(bison::key_t object_id);

  /**
   * @brief Run @p work bracketed by `on_before_dispatch`/`on_after_dispatch`.
   *
   * Obtains a stable `context&` via a brief `ctx_` lock that is released
   * before the hooks or @p work run, then calls `on_before_dispatch(ctx)`,
   * runs @p work, and calls `on_after_dispatch(ctx)` -- even if @p work
   * throws.  Called from the worker thread for every queued operation
   * (`instantiate` and each `send_request` op).
   */
  bison::dynamic dispatch(const std::function<bison::dynamic()>& work);

  // ── State ─────────────────────────────────────────────────────────────────

  /**
   * @brief Per-instance server context holding all live objects and the
   *        event-emission callback, wrapped for thread-safe external access.
   */
  bison::synchronized<context> ctx_;

  /**
   * @brief Event handlers keyed first by object ID then by event name,
   *        wrapped for thread-safe concurrent registration and dispatch.
   */
  bison::synchronized<
      std::unordered_map<bison::hash_t, std::unordered_map<bison::hash_t, std::function<void(bison::dynamic)>>>>
      event_handlers_;

  bison::synchronized<std::queue<task_item>> queue_;
  std::atomic<bool> running_{false};
  std::thread worker_;

  /// Guards one-time firing of on_session_created()/on_session_destroyed().
  std::atomic<bool> session_created_{false};
  std::atomic<bool> session_destroyed_{false};
};

} // namespace bdg::bison::rmi
