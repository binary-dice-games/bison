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

#include "src/core/bison.hpp"
#include "src/rmi/client/proxy.hpp"
#include "src/rmi/server/context.hpp"

#include <functional>
#include <future>
#include <memory>
#include <unordered_map>

namespace bdg::bison::rmi {

/**
 * @brief In-process RMI runtime that combines client and server logic without
 *        serialization or transport overhead.
 *
 * Unlike the transport-backed `client`/`server` pair, `standalone` operates
 * directly on live `bison::dynamic` object references held in an internal
 * server `context`.  There is no wire protocol, no background worker thread,
 * and no serialization; every operation executes synchronously on the calling
 * thread and the returned futures are already resolved before they are
 * returned to the caller.
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
 * Individual `standalone` instances are **not** thread-safe.  Do not share
 * a single instance across threads without external synchronization.
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
   * There is no transport to open in standalone mode; this function always
   * returns immediately without error.
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
  std::future<bison::dynamic> describe(bison::key_t klass = 0U);

  /**
   * @brief Instantiate an object from the local class registry.
   *
   * Looks up @p klass in the global `bison::dynamic` registry, creates an
   * instance, invokes the optional `__construct` hook with @p params, stores
   * the object in the internal session context, and returns a proxy that owns
   * the object ID.
   *
   * @param klass  Class key to instantiate.
   * @param params Optional constructor parameters forwarded to `__construct`.
   * @return Future already resolved with a `proxy::dynamic` owning the new
   *         object.
   * @throws std::runtime_error if the class is not registered.
   */
  std::future<proxy::dynamic> instantiate(
      bison::key_t klass,
      bison::dynamic params = bison::dynamic{});

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
   * @brief No-op compatibility shim for code that calls `disconnect` generically.
   *
   * There is no transport to close in standalone mode; this function always
   * returns immediately without error.
   */
  void disconnect();

  /**
   * @brief Dispatch a protocol operation directly on the stored objects.
   *
   * This is the backend method used by `proxy::dynamic` to implement
   * `clear`, `set`, `get`, and `call` without involving any transport layer.
   *
   * @param op        Operation token (`OP_CLEAR`, `OP_SET`, `OP_GET`,
   *                  `OP_CALL`, or `OP_DESTROY`).
   * @param object_id Target object identifier issued by `instantiate`.
   * @param payload   Operation-specific payload (consumed by move).
   * @param oneway    When true, an empty result is returned without executing
   *                  any return-value logic.  For `OP_CALL` only.
   * @return Future already resolved with the operation result.
   */
  std::future<bison::dynamic> send_request(
      bison::key_t op,
      bison::key_t object_id,
      bison::dynamic payload,
      bool oneway) override;

  /**
   * @brief Register an event handler for a named event on an object.
   *
   * Handlers are invoked synchronously from within `send_request` when the
   * object's code calls `ctx_.emit_event`.
   *
   * @param object_id Object that emits the event.
   * @param name      Event name token.
   * @param handler   Callback invoked with the event payload.
   */
  void register_event_handler(
      bison::key_t object_id,
      bison::key_t name,
      std::function<void(bison::dynamic)> handler) override;

  /**
   * @brief Remove all event handlers registered for @p object_id.
   *
   * @param object_id Object whose handlers should be cleared.
   */
  void unregister_object_events(bison::key_t object_id);

 private:
  // ── Operation handlers (mirror server-side logic without transport) ───────

  bison::dynamic handle_describe(bison::key_t klass);
  bison::dynamic handle_instantiate(bison::key_t klass, bison::dynamic params);
  bison::dynamic handle_clear(bison::key_t object_id);
  bison::dynamic handle_set(bison::key_t object_id, bison::dynamic payload);
  bison::dynamic handle_get(bison::key_t object_id, bison::dynamic projection);
  bison::dynamic
  handle_call(bison::key_t object_id, bison::dynamic payload, bool oneway);
  bison::dynamic handle_destroy(bison::key_t object_id);

  // ── Helpers ───────────────────────────────────────────────────────────────

  /**
   * @brief Look up a live object or throw `std::runtime_error`.
   * @param object_id Object identifier to look up.
   * @return Reference to the stored shared pointer.
   */
  bison::dynamic_ptr& require_object(bison::key_t object_id);

  /**
   * @brief Wrap a value in an already-resolved future.
   */
  static std::future<bison::dynamic> resolved(bison::dynamic value);

  // ── State ─────────────────────────────────────────────────────────────────

  /**
   * @brief Per-instance server context holding all live objects and the
   *        event-emission callback.
   */
  context ctx_;

  /**
   * @brief Event handlers keyed first by object ID then by event name.
   */
  std::unordered_map<
      bison::hash_t,
      std::unordered_map<bison::hash_t, std::function<void(bison::dynamic)>>>
      event_handlers_;
};

} // namespace bdg::bison::rmi
