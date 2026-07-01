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
   * @brief No-op compatibility shim for code that calls `connect` generically.
   *
   * The worker thread starts in the constructor; this function always returns
   * immediately without error.
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
   * @brief No-op compatibility shim for code that calls `disconnect`
   * generically.
   *
   * There is no transport to close in standalone mode; this function always
   * returns immediately without error.
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
};

} // namespace bdg::bison::rmi
