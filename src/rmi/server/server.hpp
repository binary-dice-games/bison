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
   * @brief Override to prepend a custom description to `OP_HELP` responses.
   *
   * Default: returns an empty string (no preamble).  The server will still
   * auto-generate a listing of all registered classes with `DisplayName`
   * attributes.
   *
   * @return Free-form text prepended before the auto-generated class listing.
   */
  virtual std::string on_help_text() const { return {}; }

  /**
   * @brief Guard hook called before class-registry lookup in
   *        `handle_instantiate`.
   *
   * The default implementation checks whether @p klass is registered under
   * @p ns in the global `bison::dynamic` registry.  Override to accept
   * classes that are not locally registered — for example, `rmi::bridge`
   * returns `true` unconditionally and forwards instantiation to an upstream
   * server.
   *
   * @param ctx   Session context for the requesting client.
   * @param ns    Namespace key of the requested class.
   * @param klass Class key of the requested type.
   * @return `true` when the request should proceed to `on_create_object`.
   */
  virtual bool on_check_class(
      context& ctx,
      bison::key_t ns,
      bison::key_t klass) {
    (void)ctx;
    auto lp = bison::dynamic::getRegistry().rlock();
    const auto& nsmap = *lp;
    auto nsIt = nsmap.find(ns);
    return nsIt != nsmap.end() && nsIt->second.count(klass);
  }

  /**
   * @brief Factory hook called when a client instantiates a new object.
   *
   * The default implementation wraps a plain `bison::dynamic::instantiate`
   * result in a `dynamic_ptr`.  Override to return a session-aware subclass
   * (e.g. a handler that holds a reference to per-session application state).
   *
   * Called from the session worker thread after `on_check_class` returns
   * `true`, outside the class-registry read lock.
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

  /**
   * @brief Called for each validated request before it is dispatched.
   *
   * Formats a human-readable trace line and forwards it to `on_print`.
   * Override to suppress or replace the default formatting.
   *
   * @param ctx  Session context for the requesting client.
   * @param env  Decoded and validated request envelope.
   */
  virtual void on_request_trace(context& ctx, const shared::envelope& env);

  /**
   * @brief Called just before each response or error frame is sent.
   *
   * Formats a human-readable trace line and forwards it to `on_print`.
   * Override to suppress or replace the default formatting.
   *
   * @param ctx              Session context for the requesting client.
   * @param request_env      The original request envelope that triggered this
   *                         reply.
   * @param op               Operation token echoed in the response.
   * @param is_error         `true` when the response is an error frame.
   * @param error_code       Canonical error code token; zero when `is_error`
   *                         is `false`.
   * @param response_payload The response payload object (empty on errors).
   */
  virtual void on_response_trace(
      context& ctx,
      const shared::envelope& request_env,
      bison::key_t op,
      bool is_error,
      bison::key_t error_code,
      const bison::dynamic& response_payload);

  /**
   * @brief Output hook for formatted trace lines.
   *
   * Called by the default `on_request_trace` and `on_response_trace`
   * implementations with a fully-formatted, human-readable string.  Override
   * to redirect trace output (e.g. to a logger or file).
   * Default: no-op.
   *
   * @param session_id  Session that produced the trace line.
   * @param line        Formatted trace string (no trailing newline).
   */
  virtual void on_print(bison::key_t session_id, const std::string& line) {
    (void)session_id;
    (void)line;
  }

  /**
   * @brief Called on the worker thread immediately before dispatching each
   *        validated request.
   *
   * Override to acquire per-session resources (e.g. a render mutex) before
   * any request handler runs.  Paired with `on_after_dispatch`.
   *
   * @param ctx Session context for the request.
   */
  virtual void on_before_dispatch(context& ctx) { (void)ctx; }

  /**
   * @brief Called on the worker thread immediately after each request
   *        dispatch completes, whether it succeeded or threw.
   *
   * Guaranteed to be called exactly once per `on_before_dispatch` call.
   * Must not throw -- override to release resources acquired in
   * `on_before_dispatch`.
   *
   * @param ctx Session context for the request.
   */
  virtual void on_after_dispatch(context& ctx) noexcept { (void)ctx; }

 private:
  // ── Private methods (defined in server.cpp) ───────────────────────────────

  void accept_loop();
  void client_worker(
      std::unique_ptr<transport::server_connection_iface> conn);
  void join_workers();

  void send_response(
      context& ctx,
      transport::server_connection_iface& conn,
      const shared::envelope& env,
      bison::key_t op,
      bison::dynamic payload);

  void send_error(
      context& ctx,
      transport::server_connection_iface& conn,
      const shared::envelope& env,
      bison::key_t op,
      bison::key_t code,
      const std::string& message);

  void handle_request(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  void handle_connect(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  void handle_describe(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  void handle_instantiate(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  void handle_clear(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  void handle_set(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  void handle_get(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  void handle_call(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  void handle_destroy(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  void handle_disconnect(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  void handle_dictionary(
      context& ctx,
      const shared::envelope& env,
      transport::server_connection_iface& conn);

  void handle_help(
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
