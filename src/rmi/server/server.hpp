// MIT License © 2025 Binary Dice Games
/**
 * @file server.hpp
 * @brief RMI server runtime that accepts connections and dispatches requests.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/server/context.hpp"
#include "src/rmi/shared/envelope.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bdg::bison::rmi {

/**
 * @brief Hosts RMI objects over a transport and serves protocol operations.
 *
 * The server owns an accept loop and spawns worker threads for active
 * connections. Each request envelope is decoded and routed to the matching
 * operation handler (`connect`, `instantiate`, `call`, etc.).
 *
 * ## Transport
 *
 * The primary constructors accept a `unique_ptr<transport::server_transport_iface>`
 * (owning) or a `transport::server_transport_iface&` (borrowing).  Template
 * convenience constructors are also provided so that concrete transport objects
 * can be passed directly without explicit `unique_ptr` wrapping:
 *
 * ```cpp
 * // Owning (server takes sole ownership):
 * server srv{std::make_unique<socket_server_transport>("0.0.0.0", 8080)};
 *
 * // Borrowing (transport lives alongside the server):
 * memory_server_transport t;
 * server srv{t};          // server borrows t, caller may also call t.connect()
 * ```
 */
class server {
 public:
  /**
   * @brief Construct a server that takes ownership of @p transport.
   * @param transport Owned transport implementation.
   */
  explicit server(
      std::unique_ptr<transport::server_transport_iface> transport)
      : transport_(transport.release(), transport_deleter{true}) {}

  /**
   * @brief Construct a server that borrows an externally-owned transport.
   *
   * The caller must ensure @p transport outlives the server.
   *
   * @param transport Transport implementation owned by the caller.
   */
  explicit server(transport::server_transport_iface& transport)
      : transport_(&transport, transport_deleter{false}) {}

  /**
   * @brief Convenience constructor for ownable concrete transport types.
   *
   * Accepts any rvalue of a type that inherits
   * `transport::server_transport_iface` and wraps it in a `unique_ptr`.
   *
   * @tparam TTransport Concrete type that inherits `server_transport_iface`.
   */
  template <
      typename TTransport,
      std::enable_if_t<
          std::is_base_of_v<
              transport::server_transport_iface,
              std::decay_t<TTransport>> &&
              !std::is_lvalue_reference_v<TTransport>,
          int> = 0>
  explicit server(TTransport&& transport)
      : server(std::make_unique<std::decay_t<TTransport>>(
            std::move(transport))) {}

  /**
   * @brief Convenience constructor for borrowable concrete transport types.
   *
   * Accepts any lvalue of a type that inherits
   * `transport::server_transport_iface` and stores a non-owning reference.
   *
   * @tparam TTransport Concrete type that inherits `server_transport_iface`.
   */
  template <
      typename TTransport,
      std::enable_if_t<
          std::is_base_of_v<
              transport::server_transport_iface,
              std::decay_t<TTransport>>,
          int> = 0>
  explicit server(TTransport& transport)
      : server(static_cast<transport::server_transport_iface&>(transport)) {}

  server(const server&) = delete;
  server& operator=(const server&) = delete;
  server(server&&) = delete;
  server& operator=(server&&) = delete;

  ~server();

  /**
   * @brief Start listening for client connections.
   * @param params Optional transport-specific listen parameters.
   */
  void listen(bison::dynamic params = bison::dynamic{});

  /** @brief Stop accept loop, close active workers, and release resources. */
  void stop();

  /**
   * @brief Return a reference to the map of active session contexts.
   *
   * The map is keyed by session-ID hash.  Each value is a
   * `shared_ptr<context>` that remains valid as long as the connection is
   * open; the pointer is removed from the map when the connection closes.
   *
   * Access the map safely from any thread via `rlock()` and `wlock()`:
   * @code
   *   auto lp = srv.session_contexts().rlock();
   *   for (auto& [id, ctx] : *lp) { ... }
   * @endcode
   */
  bison::synchronized<
      std::unordered_map<bison::hash_t, std::shared_ptr<context>>>&
  session_contexts() {
    return session_contexts_;
  }

  /** @brief Const overload of `session_contexts()`. */
  const bison::synchronized<
      std::unordered_map<bison::hash_t, std::shared_ptr<context>>>&
  session_contexts() const {
    return session_contexts_;
  }

 protected:
  /**
   * @brief Called after a session context is created and registered.
   *
   * Fires in the worker thread, after `ctx.session_id` is assigned and the
   * context is inserted into `session_contexts_`, but before the first request
   * is processed.  Override to attach per-session state.
   *
   * @param ctx Newly registered session context.
   */
  virtual void on_session_created(context& ctx) { (void)ctx; }

  /**
   * @brief Called just before a session context is cleaned up.
   *
   * Fires in the worker thread when the connection closes, before
   * `cleanup_context` destroys the object table and before the context is
   * removed from `session_contexts_`.
   *
   * @param ctx Session context about to be destroyed.
   */
  virtual void on_session_destroyed(context& ctx) { (void)ctx; }

  /**
   * @brief Factory hook called when a client instantiates a new object.
   *
   * The default implementation wraps a plain `bison::dynamic::instantiate`
   * result in a `dynamic_ptr`.  Override to return a session-aware subclass
   * (e.g. a handler that holds a reference to per-session application state).
   *
   * Called from the session worker thread after the class has been verified as
   * registered, outside the class-registry read lock.
   *
   * @param ctx   Session context for the requesting client.
   * @param ns    Namespace key of the requested class.
   * @param klass Class key of the requested type.
   * @return Heap-allocated `dynamic` (or subclass) for the new object.
   */
  virtual bison::dynamic_ptr on_create_object(
      context& ctx,
      bison::key_t ns,
      bison::key_t klass) {
    (void)ctx;
    return bison::dynamic::create_instance(ns, klass);
  }

 private:
  // ── Private methods (defined in server.cpp) ───────────────────────────────

  void accept_loop();
  void client_worker(
      std::unique_ptr<transport::server_connection_iface> conn);
  void join_workers();

  static void send_response(
      transport::server_connection_iface& conn,
      const shared::envelope& env,
      bison::key_t op,
      bison::dynamic payload);

  static void send_error(
      transport::server_connection_iface& conn,
      const shared::envelope& env,
      bison::key_t op,
      bison::key_t code,
      const std::string& message);

  void handle_request(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  static void handle_connect(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  static void handle_describe(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  void handle_instantiate(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  static void handle_clear(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  static void handle_set(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  static void handle_get(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  static void handle_call(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  static void handle_destroy(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  static void handle_disconnect(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  static void cleanup_context(context& ctx);

  // ── Members ───────────────────────────────────────────────────────────────

  struct transport_deleter {
    bool owns;
    void operator()(transport::server_transport_iface* p) const {
      if (owns) delete p;
    }
  };
  std::unique_ptr<transport::server_transport_iface, transport_deleter>
      transport_;

  std::thread accept_thread_;
  std::atomic<bool> running_{false};

  bison::synchronized<std::vector<std::thread>> workers_;

  bison::synchronized<
      std::unordered_map<bison::hash_t, std::shared_ptr<context>>>
      session_contexts_;
};

} // namespace bdg::bison::rmi
