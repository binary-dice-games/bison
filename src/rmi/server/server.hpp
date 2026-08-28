// MIT License © 2025 Binary Dice Games
/**
 * @file server.hpp
 * @brief RMI server runtime that accepts connections and dispatches requests.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/server/auth.hpp"
#include "src/rmi/server/context.hpp"
#include "src/rmi/server/profiler_service.hpp"
#include "src/rmi/shared/envelope.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
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
  explicit server(std::unique_ptr<transport::server_transport_iface> transport)
      : transport_(transport.release(), transport_deleter{true}) {}

  /**
   * @brief Construct a server that borrows an externally-owned transport.
   *
   * The caller must ensure @p transport outlives the server.
   *
   * @param transport Transport implementation owned by the caller.
   */
  explicit server(transport::server_transport_iface& transport) : transport_(&transport, transport_deleter{false}) {}

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
          std::is_base_of_v<transport::server_transport_iface, std::decay_t<TTransport>> &&
              !std::is_lvalue_reference_v<TTransport>,
          int> = 0>
  explicit server(TTransport&& transport) : server(std::make_unique<std::decay_t<TTransport>>(std::move(transport))) {}

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
      std::enable_if_t<std::is_base_of_v<transport::server_transport_iface, std::decay_t<TTransport>>, int> = 0>
  explicit server(TTransport& transport) : server(static_cast<transport::server_transport_iface&>(transport)) {}

  server(const server&) = delete;
  server& operator=(const server&) = delete;
  server(server&&) = delete;
  server& operator=(server&&) = delete;

  virtual ~server();

  /**
   * @brief Start listening for client connections.
   *
   * @param params      Optional transport-specific listen parameters.
   * @param auth_module Optional authentication hook, evaluated once per
   *                     connection from `handle_connect()`. `nullptr`
   *                     (default) disables the feature entirely --
   *                     `handle_connect()` then behaves exactly as if
   *                     `auth_module_iface` did not exist. Deliberately not a
   *                     setter: the module is fixed for the lifetime of the
   *                     accept loop it gates, so there is no sensible way to
   *                     change it after `listen()` starts accepting
   *                     connections.
   */
  void listen(bison::dynamic params = bison::dynamic{}, auth_module_ptr auth_module = nullptr);

  /** @brief Stop accept loop, close active workers, and release resources. */
  void stop();

  /**
   * @brief Opt into Perfetto-format profiling: registers the process-wide
   *        `__BisonProfiler` singleton so clients can start/stop capture
   *        and stream trace blocks, and lets the server's own native code
   *        record via `BISON_TRACE_SCOPE`/`BISON_TRACE_INSTANT`
   *        (`src/rmi/shared/profiling.hpp`).
   *
   * Call before `listen()`. Trace files are written under @p output_dir,
   * which is server-controlled -- never derived from client input.
   *
   * @param output_dir Directory `startCapture` writes `.perfetto-trace`
   *                    files into.
   */
  void enable_profiling(std::filesystem::path output_dir);

  /**
   * @brief Control whether request/response trace lines include decoded
   *        payloads.
   *
   * When `false` (the default), trace lines carry only envelope metadata
   * (operation, session id, object id, method name). When `true`,
   * `on_request_trace()` / `on_response_trace()` also append the decoded
   * call arguments (`args=...`), `set` values, and response bodies to each
   * trace line -- useful for close debugging, but noisy enough to bloat a
   * log file, hence off by default. No effect if the trace hooks are
   * overridden.
   */
  void set_trace_payloads(bool on) {
    trace_payloads_ = on;
  }

  /**
   * @brief Control whether request/response trace lines are produced at all.
   *
   * When `true` (the default), every dispatched request runs
   * `on_request_trace()` / `on_response_trace()`, which format a
   * human-readable trace string and hand it to `on_print()`. Formatting that
   * string (an `ostringstream`, dictionary key resolves, and -- when
   * `set_trace_payloads(true)` -- a full `bison::print` of the payload) is
   * not free, so a server whose `on_print()` discards the line (or that only
   * wants traces above a certain verbosity) can call `set_trace_lines(false)`
   * to skip the hooks entirely. Independent of `set_trace_payloads()`, which
   * only controls the decoded-payload suffix once a line is being built.
   */
  void set_trace_lines(bool on) {
    trace_lines_ = on;
  }

  /**
   * @brief Start capture directly from the server's own process, without
   *        going through an RMI client/proxy call.
   *
   * No-op (returns `false`) if `enable_profiling()` was never called.
   * Idempotent if capture is already active (mirrors the RMI-facing
   * `startCapture` method's behavior).
   *
   * @param label Cosmetic label recorded with the session; not used for
   *              path construction.
   * @return `true` if capture is active after the call.
   */
  bool start_capture_now(std::string_view label = {});

  /**
   * @brief Stop capture and finalize the trace file, direct C++ call
   *        equivalent of the RMI-facing `stopCapture` method.
   *
   * No-op if profiling was never enabled or capture is already inactive.
   */
  void stop_capture_now();

  /** @brief `true` if profiling is enabled and capture is currently active. */
  bool is_capture_active_now() const;

  /**
   * @brief Per-session context holder: an individually lockable slot in
   *        `session_contexts_`.
   *
   * The `context` is held behind a `unique_ptr` (rather than embedded
   * directly in the `synchronized<>`) so that `on_create_context` may return
   * a polymorphic subclass without slicing -- `bison::synchronized<T>` stores
   * `T` by value, so `synchronized<context>` could never hold a derived type,
   * but `synchronized<unique_ptr<context>>` can.
   */
  using context_holder = std::shared_ptr<bison::synchronized<std::unique_ptr<context>>>;

  /**
   * @brief Return a reference to the map of active session contexts.
   *
   * The map is keyed by session-ID hash.  Each value is a `context_holder`
   * that remains valid as long as the connection is open; the pointer is
   * removed from the map when the connection closes.
   *
   * Each entry is individually lockable, so external readers never race the
   * owning worker thread's per-request dispatch:
   * @code
   *   auto lp = srv.session_contexts().rlock();
   *   for (auto& [id, holder] : *lp) {
   *     auto clp = holder->rlock();   // lock this one context
   *     const context& ctx = **clp;   // dereference the unique_ptr
   *     ...
   *   }
   * @endcode
   */
  bison::synchronized<std::unordered_map<bison::hash_t, context_holder>>& session_contexts() {
    return session_contexts_;
  }

  /** @brief Const overload of `session_contexts()`. */
  const bison::synchronized<std::unordered_map<bison::hash_t, context_holder>>& session_contexts() const {
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
  virtual void on_session_created(context& ctx) {
    (void)ctx;
  }

  /**
   * @brief Called just before a session context is cleaned up.
   *
   * Fires in the worker thread when the connection closes, before
   * `cleanup_context` destroys the object table and before the context is
   * removed from `session_contexts_`.
   *
   * @param ctx Session context about to be destroyed.
   */
  virtual void on_session_destroyed(context& ctx) {
    (void)ctx;
  }

  /**
   * @brief Called from `handle_connect()` once `auth_module_`'s
   *        `authenticate()` call returns `true` for this connection.
   *
   * No-op default. Only ever fires when `listen()` was given a non-null
   * `auth_module`; never fires for a rejected `authenticate()` call or when
   * no module is set. Fires before the `OP_CONNECT` ack is sent, on the
   * worker thread, with the session's context wlock already held (see
   * `client_worker`'s dispatch loop) -- override to act on the identity
   * bison itself attaches no meaning to (e.g. wish derives a persistent
   * sandbox directory from it).
   *
   * @param ctx      Session context for the newly authenticated connection.
   * @param identity Identity string produced by `auth_module_`; may be empty
   *                  if the module accepted the connection without one.
   */
  virtual void on_authenticated(context& ctx, const std::string& identity) {
    (void)ctx;
    (void)identity;
  }

  /**
   * @brief Override to prepend a custom description to `OP_HELP` responses.
   *
   * Default: returns an empty string (no preamble).  The server will still
   * auto-generate a listing of all registered classes with `DisplayName`
   * attributes.
   *
   * @return Free-form text prepended before the auto-generated class listing.
   */
  virtual std::string on_help_text() const {
    return {};
  }

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
  virtual bool on_check_class(context& ctx, bison::key_t ns, bison::key_t klass) {
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
  virtual bison::dynamic_ptr on_create_object(context& ctx, bison::key_t ns, bison::key_t klass) {
    (void)ctx;
    if (profiler_service_ && ns == shared::constants::NS_BISON && klass == shared::constants::CLASS_PROFILER)
      return bison::dynamic_ptr{std::static_pointer_cast<bison::dynamic>(profiler_service_)};
    return bison::dynamic::create_instance(ns, klass);
  }

  /**
   * @brief Factory hook called once per connection to construct the
   *        session's `context` object.
   *
   * The default implementation constructs a plain `context`.  Override to
   * return an application-specific subclass with extra per-session state
   * (e.g. `wish::server` returns a `wish::context`).  The returned object is
   * immediately wrapped in its own lockable slot and inserted into
   * `session_contexts_` before `on_session_created` fires.
   *
   * Called from the session worker thread, before the context is registered
   * or locked by anything else.
   *
   * @param session_id Freshly generated identifier for the new session.
   * @return Heap-allocated `context` (or subclass) for the new session.
   */
  virtual std::unique_ptr<context> on_create_context(bison::key_t session_id) {
    return std::make_unique<context>(session_id);
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
   *        validated request, and once more around session teardown
   *        (`on_session_destroyed()` + `cleanup_context()`, which runs
   *        object destructors -- see `client_worker()`).
   *
   * Override to acquire per-session resources (e.g. a render mutex) before
   * any request handler runs, or more generally to mark (e.g. via a
   * thread-local) that the calling thread already holds `ctx`'s wlock for
   * the duration of the call -- both request dispatch and teardown run with
   * it held. Paired with `on_after_dispatch`.
   *
   * @param ctx Session context for the request, or being torn down.
   */
  virtual void on_before_dispatch(context& ctx) {
    (void)ctx;
  }

  /**
   * @brief Called on the worker thread immediately after each request
   *        dispatch completes (whether it succeeded or threw), and once
   *        more after session teardown finishes -- see `on_before_dispatch`.
   *
   * Guaranteed to be called exactly once per `on_before_dispatch` call.
   * Must not throw -- override to release resources acquired in
   * `on_before_dispatch`.
   *
   * @param ctx Session context for the request, or being torn down.
   */
  virtual void on_after_dispatch(context& ctx) noexcept {
    (void)ctx;
  }

  /**
   * @brief Called at the very start of `handle_request()`, before the
   *        built-in `ctx.objects`-based dispatch (and before
   *        `on_request_trace()`).
   *
   * If this returns `true`, the request is considered fully handled --
   * the override must itself have sent a response or error (via
   * `send_response()`/`send_error()`) -- and `handle_request()` returns
   * immediately without running any of the built-in `OP_*` handlers or the
   * trace hooks.
   *
   * Lets a subclass implement request handling that doesn't fit the
   * per-connection `ctx.objects` model -- e.g. `bridge` relays every object
   * operation straight to a single shared upstream connection instead of
   * storing objects locally, so it has nothing for the generic dispatch to
   * look up. The base implementation returns `false` (no-op), so ordinary
   * `server` subclasses are unaffected.
   *
   * @param ctx  Session context for the connection @p env arrived on.
   * @param env  Decoded, already-validated request envelope.
   * @param conn Connection to reply on if this returns `true`.
   * @return `true` if @p env was fully handled (skip the generic dispatch).
   */
  virtual bool try_handle_request(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn) {
    (void)ctx;
    (void)env;
    (void)conn;
    return false;
  }

  /**
   * @brief Send a protocol response envelope.
   *
   * Exposed to subclasses so a `try_handle_request()` override can reply
   * using the same framing as the built-in `OP_*` handlers.
   */
  void send_response(
      context& ctx,
      transport::server_connection_iface& conn,
      const shared::envelope& env,
      bison::key_t op,
      bison::dynamic payload);

  /**
   * @brief Send a protocol error envelope.
   *
   * Exposed to subclasses so a `try_handle_request()` override can report a
   * failure using the same framing as the built-in `OP_*` handlers.
   */
  void send_error(
      context& ctx,
      transport::server_connection_iface& conn,
      const shared::envelope& env,
      bison::key_t op,
      bison::key_t code,
      const std::string& message);

 private:
  /**
   * @brief One connection's dispatch state, owned by exactly one worker in
   *        `dispatch_workers_`. See that member's doc comment.
   */
  struct session_slot {
    std::unique_ptr<transport::server_connection_iface> conn;
    context_holder ctx_holder;
    bison::key_t session_id;
  };

  // ── Private methods (defined in server.cpp) ───────────────────────────────

  void accept_loop();
  void start_session(std::unique_ptr<transport::server_connection_iface> conn, size_t worker_index);
  void dispatch_worker(size_t worker_index);
  void service_session(session_slot& slot);
  void teardown_session(session_slot& slot);
  void join_workers();

  void handle_request(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn);

  void handle_connect(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn);

  void handle_describe(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn);

  void handle_instantiate(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn);

  void handle_clear(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn);

  void handle_set(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn);

  void handle_get(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn);

  void handle_call(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn);

  void handle_destroy(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn);

  /**
   * @brief Destroy every object filed under `env.group` (see
   *        `context::groups`), running each one's `__destruct` hook first.
   *        A no-op success if the group is empty or unknown -- not an error.
   */
  void handle_destroy_group(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn);

  void handle_disconnect(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn);

  void handle_dictionary(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn);

  void handle_help(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn);

  static void cleanup_context(context& ctx);

  // ── Members ───────────────────────────────────────────────────────────────

  struct transport_deleter {
    bool owns;
    void operator()(transport::server_transport_iface* p) const {
      if (owns)
        delete p;
    }
  };
  std::unique_ptr<transport::server_transport_iface, transport_deleter> transport_;

  std::thread accept_thread_;
  std::atomic<bool> running_{false};

  /** @brief Set once by `listen()`; read-only for the rest of the server's
   *         lifetime (see `listen()`'s `auth_module` parameter doc). */
  auth_module_ptr auth_module_;

  /**
   * @brief One dispatch worker: a single OS thread servicing a bounded
   *        subset of live sessions, instead of every connection getting its
   *        own dedicated thread.
   *
   * `accept_loop()` assigns each newly-accepted connection to one worker
   * (round robin, see `start_session()`). That worker's thread
   * (`dispatch_worker()`) repeatedly cycles through its own `sessions`,
   * calling `session_slot::conn->receive()` with a short timeout
   * (`kDispatchPollTimeout`) for each in turn -- a real (non-busy)
   * condition-variable wait per session that returns immediately if data
   * has already arrived, or after the timeout if not, before moving on to
   * the next session in this worker's list. A session assigned to a worker
   * is never touched by any other thread, so per-session request ordering
   * and the existing `ctx_holder`-based locking are unaffected -- only the
   * OS thread issuing `receive()` calls for it is now shared across many
   * sessions rather than dedicated to just one.
   *
   * Tradeoff versus one thread per connection: this bounds total dispatch
   * thread count (and therefore memory/scheduling cost) to a small,
   * fixed number regardless of how many sessions are live, at the cost of
   * added worst-case per-request latency that grows with how many sessions
   * share a worker (bounded by roughly `kDispatchPollTimeout ×
   * sessions-per-worker` when everything on a worker happens to be idle in
   * sequence) -- see kDispatchPollTimeout's own doc comment. This works
   * uniformly across every transport (TCP, TLS, named pipe, memory,
   * stream, term) because it only uses the existing blocking
   * `transport::server_connection_iface::receive(timeout)` contract, not
   * anything transport-specific.
   */
  struct dispatch_worker_state {
    std::thread thread;
    bison::synchronized<std::vector<std::shared_ptr<session_slot>>> sessions;
  };

  /**
   * @brief Per-session poll timeout used by `dispatch_worker()`'s round
   *        robin over its assigned sessions.
   *
   * A real wait (via `synchronized<>::wait_for`'s condition variable), not
   * a busy-spin: a session with data ready returns immediately, so this
   * value only bounds how long an *idle* session is waited on before the
   * worker moves on to give the next assigned session its turn. Smaller
   * values reduce added latency per session at the cost of more wake/sleep
   * cycles (and therefore more CPU) when most sessions on a worker are
   * idle; larger values do the reverse. 5ms is a modest default that keeps
   * worst-case added latency in the low hundreds of milliseconds even at a
   * few hundred sessions per worker, while not spinning noticeably on an
   * idle server.
   */
  static constexpr std::chrono::milliseconds kDispatchPollTimeout{5};

  std::vector<std::unique_ptr<dispatch_worker_state>> dispatch_workers_;

  bison::synchronized<std::unordered_map<bison::hash_t, context_holder>> session_contexts_;

  /** @brief Set by `enable_profiling()`; null unless profiling was opted into. */
  std::shared_ptr<profiler_service> profiler_service_;

  /** @brief Whether trace lines carry decoded payloads; see `set_trace_payloads()`. */
  bool trace_payloads_ = false;

  /** @brief Whether the trace hooks run at all; see `set_trace_lines()`. */
  bool trace_lines_ = true;
};

} // namespace bdg::bison::rmi
