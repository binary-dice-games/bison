// MIT License © 2025 Binary Dice Games
/**
 * @file client.hpp
 * @brief RMI client runtime for request/response and event dispatch.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"
#include "src/rmi/shared/constants.hpp"
#include "src/rmi/shared/envelope.hpp"
#include "src/rmi/shared/ids.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bdg::bison::rmi {

/**
 * @brief Owns a transport connection and performs RMI protocol operations.
 *
 * The client serializes requests into RMI envelopes and runs two background
 * threads:
 *
 * - **Worker thread** — reads frames from the transport, resolves pending
 *   response futures by request ID, and enqueues incoming events.
 * - **Event dispatch thread** — drains the event queue and calls user
 *   registered handlers one at a time (FIFO).  Because this thread is
 *   independent of the worker thread, event handlers may safely call any
 *   blocking RMI operation (e.g. `send_request(...).get()`) without
 *   deadlocking.
 *
 * Inherits `proxy_backend` so that `proxy::dynamic` instances created by this
 * client can dispatch their operations back through the same interface as
 * in-process `standalone` sessions.
 *
 * ## Transport
 *
 * The primary constructor accepts a
 * `unique_ptr<transport::client_transport_iface>` giving the caller full
 * control over which transport is used (TCP socket, stdio, in-memory, or any
 * custom implementation).  A template convenience constructor is also provided
 * so concrete transport objects can be passed directly and are automatically
 * wrapped in a `unique_ptr`:
 *
 * ```cpp
 * // Explicit ownership transfer:
 * client c{std::make_unique<socket_client_transport>("127.0.0.1", 8080)};
 *
 * // Convenience (concrete type is inferred):
 * socket_client_transport t{"127.0.0.1", 8080};
 * client c{std::move(t)};
 * ```
 */
class client : public proxy_backend {
 public:
  /**
   * @brief Construct a client that takes ownership of @p transport.
   * @param transport Transport implementation to use.
   */
  explicit client(std::unique_ptr<transport::client_transport_iface> transport) : transport_(std::move(transport)) {}

  /**
   * @brief Convenience constructor for concrete transport types.
   *
   * Accepts any type that is (or inherits) `transport::client_transport_iface`
   * and wraps it in a `unique_ptr` automatically.  This keeps call sites that
   * pass concrete transport values unchanged.
   *
   * @tparam TTransport Concrete type that inherits `client_transport_iface`.
   */
  template <
      typename TTransport,
      std::enable_if_t<std::is_base_of_v<transport::client_transport_iface, std::decay_t<TTransport>>, int> = 0>
  explicit client(TTransport&& transport)
      : client(std::make_unique<std::decay_t<TTransport>>(std::forward<TTransport>(transport))) {}

  client(const client&) = delete;
  client& operator=(const client&) = delete;
  client(client&&) = delete;
  client& operator=(client&&) = delete;

  virtual ~client();

  /**
   * @brief Called at the end of `connect()` once the handshake succeeds.
   *
   * Fires in the thread that called `connect()`.  Override to initialise
   * per-session state (e.g. instantiate protocol proxies).
   */
  virtual void on_connect() {}

  /**
   * @brief Called inside the `instantiate()` continuation after the proxy is
   *        created but before the future is resolved for the caller.
   *
   * Fires in the detached async thread that resolves the `instantiate()`
   * future.  Override to track live remote objects or register event handlers.
   *
   * @param proxy The newly created proxy (valid, not yet returned to caller).
   */
  virtual void on_instantiate(const proxy::dynamic& proxy) {
    (void)proxy;
  }

  /**
   * @brief Called at the start of `destroy()`, before the proxy is invalidated
   *        and before `OP_DESTROY` is sent.
   *
   * Fires in the thread that called `destroy()`.  Override to clean up state
   * associated with the remote object (e.g. remove from a tracking registry).
   *
   * @param object_id The ID of the object about to be destroyed.
   */
  virtual void on_destroy(bison::key_t object_id) {
    (void)object_id;
  }

  /**
   * @brief Called inside `disconnect()` after the worker thread has stopped
   *        and the transport has shut down, before pending requests are failed.
   *
   * Fires in the thread that called `disconnect()`.  Override to release any
   * state that is only valid during an active session (e.g. proxy handles).
   *
   * @note Not called when the destructor triggers `disconnect()` as a safety
   *       net — virtual dispatch is unavailable at that point.
   */
  virtual void on_disconnect() {}

  /**
   * @brief Open the transport and start the client worker loop.
   * @param params Optional transport-specific startup parameters.
   */
  void connect(bison::dynamic params = bison::dynamic{});

  /**
   * @brief Request class metadata from the server.
   * @param ns Namespace key; `0U` selects the global namespace.
   * @param klass Optional class key. Pass `0` to request full metadata.
   * @return Future resolved with the description payload from the server.
   */
  std::future<bison::dynamic> describe(bison::key_t ns = 0U, bison::key_t klass = 0U);

  /**
   * @brief Request the server's hash→display-name dictionary.
   *
   * The returned `bison::dynamic` maps each registered item's `key_t` hash to
   * its `DisplayName` string.  Only items with a `DisplayName` attribute are
   * included.  The dictionary is flat: class keys, field keys, and method keys
   * are all at the top level.
   *
   * @return Future resolved with the dictionary payload.
   */
  std::future<bison::dynamic> get_dictionary();

  /**
   * @brief Request human-readable help text describing the server.
   *
   * The returned `bison::dynamic` contains a `FIELD_DESCRIPTION` string with
   * an auto-generated listing of registered classes, their fields, and their
   * methods (display names and descriptions only).  The server may prepend a
   * custom intro via `on_help_text()`.
   *
   * @return Future resolved with the help payload.
   */
  std::future<bison::dynamic> get_help();

  /**   * @brief Create a remote object instance on the server.
   * @param ns Class namespace key; `0U` selects the global namespace.
   * @param klass Class key to instantiate.
   * @param params Optional constructor parameters.
   * @return Future resolved with a move-only proxy referencing the server-side
   * object.
   */
  std::future<proxy::dynamic>
  instantiate(bison::key_t ns, bison::key_t klass, bison::dynamic params = bison::dynamic{});

  /**
   * @brief Destroy a remote object represented by @p proxy.
   * @param proxy Owning proxy to invalidate and destroy remotely.
   */
  void destroy(proxy::dynamic&& proxy);

  /** @brief Gracefully disconnect from the server and stop worker threads. */
  void disconnect();

  /**
   * @brief Send a low-level RMI request.
   * @param op Operation key.
   * @param object_id Target object ID (empty for non-object operations).
   * @param payload Operation payload object.
   * @param oneway When true, no response is expected from the server.
   * @return Future resolved with response payload (or empty dynamic for
   * oneway).
   */
  std::future<bison::dynamic> send_request(bison::key_t op, bison::key_t object_id, bison::dynamic payload, bool oneway)
      override;

  /**
   * @brief Register a handler for server-sent events on an object.
   * @param object_id Remote object identifier.
   * @param name Event name token.
   * @param handler Callback invoked with event payload.
   */
  void register_event_handler(bison::key_t object_id, bison::key_t name, std::function<void(bison::dynamic)> handler)
      override;

  /**
   * @brief Remove all event handlers associated with an object ID.
   * @param object_id Remote object identifier.
   */
  void unregister_object_events(bison::key_t object_id);

  /**
   * @brief Dispatch an already-decoded event envelope through this client's
   *        own registered handlers, bypassing the transport receive loop.
   *
   * Used by `rmi::bridge` to deliver events for objects it instantiated for
   * its own use (not on behalf of a downstream session) -- its
   * event-intercepting upstream transport consumes every `KIND_EVENT` frame
   * before this client's worker thread would otherwise see it, so those
   * events need an alternate entry point into `event_handlers_` dispatch.
   * @param env Decoded envelope; must have `kind == KIND_EVENT`.
   */
  void dispatch_local_event(const shared::envelope& env) {
    process_frame(env);
  }

  /**
   * @brief Create a proxy for a server-side object whose ID is already known.
   *
   * Use this when the server returns an object ID through a method call result
   * rather than through `OP_INSTANTIATE`.  The caller is responsible for
   * ensuring @p id refers to a live object in the current session.
   *
   * @param id Remote object identifier.
   * @return A valid `proxy::dynamic` backed by this client.
   */
  proxy::dynamic make_proxy(bison::key_t id) {
    return proxy::dynamic(this, std::move(id));
  }

 private:
  // ── Private methods (defined in client.cpp) ───────────────────────────────

  void worker_loop();
  void event_loop();
  void process_frame(const shared::envelope& env);
  void fail_all_pending(bison::key_t code, const std::string& message);

  // ── Members ───────────────────────────────────────────────────────────────

  bison::synchronized<std::unique_ptr<transport::client_transport_iface>> transport_;
  std::thread worker_;
  std::thread event_thread_;
  std::atomic<bool> running_{false};

  // Event dispatch queue — worker produces, event_thread_ consumes.
  bison::synchronized<std::queue<std::function<void()>>> event_queue_;

  bison::synchronized<std::unordered_map<bison::hash_t, std::promise<bison::dynamic>>> pending_;

  bison::synchronized<
      std::unordered_map<bison::hash_t, std::unordered_map<bison::hash_t, std::function<void(bison::dynamic)>>>>
      event_handlers_;
};

} // namespace bdg::bison::rmi
