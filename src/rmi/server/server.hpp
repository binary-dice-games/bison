// MIT License © 2025 Binary Dice Games
#pragma once

#include "src/core/bison.hpp"
#include "src/rmi/server/context.hpp"
#include "src/rmi/shared/constants.hpp"
#include "src/rmi/shared/envelope.hpp"
#include "src/rmi/shared/ids.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace bdg::bison::rmi {

/**
 * @brief RMI server.
 *
 * Accepts client connections over a pluggable transport, spawns a dedicated
 * worker thread for each client, and dispatches protocol operations to
 * server-side `bison::dynamic` objects stored in per-session contexts.
 *
 * ### Usage
 * ```cpp
 * memory_server_transport transport;
 * server s{transport};          // or server s{std::move(transport)};
 * s.listen(bison::dynamic{});
 *
 * auto client_transport = transport.connect(); // test side
 * // … use client …
 *
 * s.stop();
 * ```
 */
class server {
 public:
  // ── Construction ─────────────────────────────────────────────────────────

  template <typename TTransport>
  explicit server(TTransport& transport)
      : transport_(std::make_unique<transport_wrapper<TTransport>>(transport)) {}

  template <typename TTransport>
  explicit server(TTransport&& transport)
      : transport_(std::make_unique<transport_wrapper<std::decay_t<TTransport>>>(
            std::move(transport))) {}

  server(const server&)            = delete;
  server& operator=(const server&) = delete;
  server(server&&)                 = delete;
  server& operator=(server&&)      = delete;

  ~server() {
    if (running_.load()) {
      try { stop(); } catch (...) {}
    }
  }

  // ── Public API ────────────────────────────────────────────────────────────

  /**
   * @brief Start the accept loop and initialise the transport.
   *
   * @param params  Transport-specific listen parameters (e.g. port).
   */
  void listen(bison::dynamic params = bison::dynamic{}) {
    shared::register_envelope();
    running_.store(true);
    transport_->start(std::move(params));
    accept_thread_ = std::thread(&server::accept_loop, this);
  }

  /**
   * @brief Stop the server: terminate accept loop, close all connections, join
   *        all worker threads.
   */
  void stop() {
    running_.store(false);
    transport_->stop();
    if (accept_thread_.joinable()) accept_thread_.join();
    join_workers();
  }

 private:
  // ── Type-erased transport ─────────────────────────────────────────────────

  struct connection_iface {
    virtual void send(std::vector<char> frame)                          = 0;
    virtual bool receive(std::vector<char>&,
                         std::chrono::milliseconds)                     = 0;
    virtual void close()                                                = 0;
    virtual bool is_closed() const                                      = 0;
    virtual ~connection_iface()                                         = default;
  };

  template <typename C>
  struct connection_wrapper final : connection_iface {
    explicit connection_wrapper(C&& c) : c_(std::move(c)) {}
    void send(std::vector<char> f) override  { c_.send(std::move(f)); }
    bool receive(std::vector<char>& f,
                 std::chrono::milliseconds to) override {
      return c_.receive(f, to);
    }
    void close() override          { c_.close(); }
    bool is_closed() const override { return c_.is_closed(); }
    C c_;
  };

  struct transport_iface {
    virtual void start(bison::dynamic params)                           = 0;
    virtual std::unique_ptr<connection_iface> accept(
        std::chrono::milliseconds timeout)                              = 0;
    virtual void stop()                                                 = 0;
    virtual ~transport_iface()                                          = default;
  };

  // Wrapper that holds the transport by reference (for listen-on-stack usage).
  template <typename T>
  struct transport_wrapper final : transport_iface {
    explicit transport_wrapper(T& t) : t_(t) {}
    void start(bison::dynamic p) override { t_.start(std::move(p)); }
    std::unique_ptr<connection_iface> accept(
        std::chrono::milliseconds to) override {
      auto maybe = t_.accept(to);
      if (!maybe) return nullptr;
      using ConnType = typename decltype(maybe)::value_type;
      return std::make_unique<connection_wrapper<ConnType>>(
          std::move(*maybe));
    }
    void stop() override { t_.stop(); }
    T& t_;
  };

  // ── Accept loop ───────────────────────────────────────────────────────────

  void accept_loop() {
    while (running_.load(std::memory_order_acquire)) {
      auto conn = transport_->accept(std::chrono::milliseconds{100});
      if (!conn) continue;

      std::lock_guard<std::mutex> lk(workers_mutex_);
      workers_.emplace_back(
          std::thread(&server::client_worker, this, std::move(conn)));
    }
  }

  // ── Client worker ─────────────────────────────────────────────────────────

  void client_worker(std::unique_ptr<connection_iface> conn) {
    server::context ctx;

    // Provide emit_event so objects can fire server-initiated events.
    ctx.emit_event = [&conn](const std::string& oid, bison::key_t name,
                              bison::dynamic params) {
      using namespace shared::constants;
      bison::dynamic payload;
      payload[FIELD_NAME]   = name;
      payload[FIELD_PARAMS] = std::make_shared<bison::dynamic>(std::move(params));
      const std::string payload_bytes = shared::encode_payload(payload);
      auto frame = shared::encode_envelope(KIND_EVENT, OP_EVENT,
                                           {}, oid, true, payload_bytes);
      conn->send(std::move(frame));
    };

    while (!conn->is_closed()) {
      std::vector<char> frame;
      if (!conn->receive(frame, std::chrono::milliseconds{50})) continue;

      try {
        auto env = shared::decode_envelope(frame);
        handle_request(ctx, *env, *conn);
      } catch (const std::exception& e) {
        // Send a best-effort INVALID_REQUEST error and continue.
        try {
          const std::string err =
              shared::encode_error(shared::constants::ERR_INVALID_REQUEST,
                                   e.what());
          auto resp = shared::encode_envelope(
              shared::constants::KIND_RESPONSE,
              shared::constants::OP_CONNECT, {}, {}, false, {}, err);
          conn->send(std::move(resp));
        } catch (...) {}
      }
    }

    cleanup_context(ctx, *conn);
  }

  // ── Helpers ───────────────────────────────────────────────────────────────

  void join_workers() {
    std::lock_guard<std::mutex> lk(workers_mutex_);
    for (auto& t : workers_) {
      if (t.joinable()) t.join();
    }
    workers_.clear();
  }

  static void send_response(connection_iface&  conn,
                             const bison::dynamic& env,
                             bison::key_t          op,
                             const std::string&    payload_bytes,
                             const std::string&    error_bytes = {}) {
    using namespace shared::constants;
    std::string request_id = env[FIELD_REQUEST_ID];
    std::string object_id  = env[FIELD_OBJECT_ID];
    auto frame = shared::encode_envelope(KIND_RESPONSE, op, request_id,
                                         object_id, false,
                                         payload_bytes, error_bytes);
    conn.send(std::move(frame));
  }

  static void send_error(connection_iface&     conn,
                         const bison::dynamic& env,
                         bison::key_t          op,
                         bison::key_t          code,
                         const std::string&    message) {
    send_response(conn, env, op, {}, shared::encode_error(code, message));
  }

  // ── Operation dispatch ────────────────────────────────────────────────────

  void handle_request(server::context&   ctx,
                      const bison::dynamic& env,
                      connection_iface&  conn) {
    using namespace shared::constants;

    // Validate protocol version.
    int32_t version = env[FIELD_VERSION];
    if (version != PROTOCOL_VERSION) {
      send_error(conn, env, OP_CONNECT, ERR_UNSUPPORTED_VERSION,
                 "Unsupported protocol version");
      return;
    }

    bison::key_t op = env[FIELD_OP];

    if      (op == OP_CONNECT)     handle_connect(ctx, env, conn);
    else if (op == OP_DESCRIBE)    handle_describe(ctx, env, conn);
    else if (op == OP_INSTANTIATE) handle_instantiate(ctx, env, conn);
    else if (op == OP_CLEAR)       handle_clear(ctx, env, conn);
    else if (op == OP_SET)         handle_set(ctx, env, conn);
    else if (op == OP_GET)         handle_get(ctx, env, conn);
    else if (op == OP_CALL)        handle_call(ctx, env, conn);
    else if (op == OP_DESTROY)     handle_destroy(ctx, env, conn);
    else if (op == OP_DISCONNECT)  handle_disconnect(ctx, env, conn);
    else {
      send_error(conn, env, op, ERR_UNKNOWN_OPERATION, "Unknown operation");
    }
  }

  // ── Individual operation handlers ─────────────────────────────────────────

  static void handle_connect(server::context&      /*ctx*/,
                              const bison::dynamic& env,
                              connection_iface&      conn) {
    using namespace shared::constants;
    bison::dynamic resp;
    resp[FIELD_VERSION] = int32_t{PROTOCOL_VERSION};
    send_response(conn, env, OP_CONNECT, shared::encode_payload(resp));
  }

  static void handle_describe(server::context&      /*ctx*/,
                               const bison::dynamic& env,
                               connection_iface&      conn) {
    using namespace shared::constants;

    const std::string payload_bytes = env[FIELD_PAYLOAD];
    auto              payload       = shared::decode_payload(payload_bytes);

    bison::key_t requested_klass = (*payload)[FIELD_KLASS];

    bison::dynamic resp;

    std::shared_lock<std::shared_mutex> lk(bison::dynamic::getMutex());
    auto& classes = bison::dynamic::getClasses();

    if (requested_klass == bison::key_t{0u}) {
      // Return all (non-internal) classes as an array.
      size_t idx = 0;
      for (const auto& [klass, proto] : classes) {
        if (klass == CLASS_ENVELOPE) continue;
        bison::dynamic desc;
        desc[FIELD_KLASS] = klass;
        resp[idx++] = std::make_shared<bison::dynamic>(std::move(desc));
      }
    } else {
      auto it = classes.find(requested_klass);
      if (it == classes.end()) {
        lk.unlock();
        send_error(conn, env, OP_DESCRIBE, ERR_CLASS_NOT_FOUND,
                   "Class not found");
        return;
      }
      resp[FIELD_KLASS] = requested_klass;
      // Copy prototype fields as descriptors.
      it->second->forEach([&resp](bison::key_t k, const bison::field& v) {
        resp[k] = v;
      });
    }

    send_response(conn, env, OP_DESCRIBE, shared::encode_payload(resp));
  }

  static void handle_instantiate(server::context&      ctx,
                                  const bison::dynamic& env,
                                  connection_iface&      conn) {
    using namespace shared::constants;

    const std::string payload_bytes = env[FIELD_PAYLOAD];
    auto              payload       = shared::decode_payload(payload_bytes);

    bison::key_t klass = (*payload)[FIELD_KLASS];

    // Verify the class is registered.
    {
      std::shared_lock<std::shared_mutex> lk(bison::dynamic::getMutex());
      if (!bison::dynamic::getClasses().count(klass)) {
        send_error(conn, env, OP_INSTANTIATE, ERR_CLASS_NOT_FOUND,
                   "Class not registered on server");
        return;
      }
    }

    auto obj = std::make_shared<bison::dynamic>(bison::dynamic::instantiate(klass));

    // Invoke __construct hook if present.
    if (obj->findMethod(HOOK_CONSTRUCT) != nullptr) {
      try {
        bison::dynamic construct_params;
        auto& pf = (*payload)[FIELD_PARAMS];
        if (pf.is<std::shared_ptr<bison::dynamic>>()) {
          auto ptr = static_cast<std::shared_ptr<bison::dynamic>>(pf);
          if (ptr) construct_params = std::move(*ptr);
        }
        obj->call(HOOK_CONSTRUCT, construct_params);
      } catch (const std::exception& e) {
        send_error(conn, env, OP_INSTANTIATE, ERR_INTERNAL_ERROR,
                   std::string("__construct failed: ") + e.what());
        return;
      }
    }

    const std::string oid = shared::generate_id();
    ctx.objects[oid] = obj;

    bison::dynamic resp;
    resp[FIELD_OBJECT_ID] = oid;
    resp[FIELD_KLASS]     = klass;
    send_response(conn, env, OP_INSTANTIATE, shared::encode_payload(resp));
  }

  static void handle_clear(server::context&      ctx,
                            const bison::dynamic& env,
                            connection_iface&      conn) {
    using namespace shared::constants;

    std::string oid = env[FIELD_OBJECT_ID];
    auto it = ctx.objects.find(oid);
    if (it == ctx.objects.end()) {
      send_error(conn, env, OP_CLEAR, ERR_OBJECT_NOT_FOUND, "Object not found");
      return;
    }
    auto& obj = *it->second;

    // Clear explicitly set named fields (keep CLASS).
    obj.forEach([&obj](bison::key_t k, const bison::field& /*v*/) {
      if (k != bison::dynamic::CLASS && k != bison::dynamic::PARENT) {
        // Re-use numeric-clear variant: erase by rebuilding; not available
        // directly. Instead, overwrite with monostate via a fresh dynamic.
      }
    });
    // Rebuild the object retaining only meta fields.
    bison::key_t klass_key = obj[bison::dynamic::CLASS];
    *it->second = bison::dynamic::instantiate(klass_key);
    auto& fresh = *it->second;

    // Invoke __clear hook if present.
    if (fresh.findMethod(HOOK_CLEAR) != nullptr) {
      try { fresh.call(HOOK_CLEAR, bison::dynamic{}); }
      catch (...) {}
    }

    send_response(conn, env, OP_CLEAR, {});
  }

  static void handle_set(server::context&      ctx,
                          const bison::dynamic& env,
                          connection_iface&      conn) {
    using namespace shared::constants;

    std::string oid = env[FIELD_OBJECT_ID];
    auto it = ctx.objects.find(oid);
    if (it == ctx.objects.end()) {
      send_error(conn, env, OP_SET, ERR_OBJECT_NOT_FOUND, "Object not found");
      return;
    }
    auto& obj = *it->second;

    const std::string payload_bytes = env[FIELD_PAYLOAD];
    auto              patch         = shared::decode_payload(payload_bytes);

    // Apply __setter hook if present — hook transforms the incoming patch.
    if (obj.findMethod(HOOK_SETTER) != nullptr) {
      try { *patch = obj.call(HOOK_SETTER, *patch); }
      catch (const std::exception& e) {
        send_error(conn, env, OP_SET, ERR_INTERNAL_ERROR,
                   std::string("__setter failed: ") + e.what());
        return;
      }
    }

    // Apply field patch (skip meta fields).
    patch->forEach([&obj](bison::key_t k, const bison::field& v) {
      if (k != bison::dynamic::CLASS && k != bison::dynamic::PARENT) {
        obj[k] = v;
      }
    });

    send_response(conn, env, OP_SET, {});
  }

  static void handle_get(server::context&      ctx,
                          const bison::dynamic& env,
                          connection_iface&      conn) {
    using namespace shared::constants;

    std::string oid = env[FIELD_OBJECT_ID];
    auto it = ctx.objects.find(oid);
    if (it == ctx.objects.end()) {
      send_error(conn, env, OP_GET, ERR_OBJECT_NOT_FOUND, "Object not found");
      return;
    }
    auto& obj = *it->second;

    const std::string payload_bytes = env[FIELD_PAYLOAD];
    auto              projection    = shared::decode_payload(payload_bytes);

    // Check whether the projection carries any user fields.
    bool has_projection = false;
    projection->forEach([&](bison::key_t k, const bison::field&) {
      if (k != bison::dynamic::CLASS && k != bison::dynamic::PARENT)
        has_projection = true;
    });

    bison::dynamic result;
    if (!has_projection) {
      // Full snapshot.
      result = obj.clone();
    } else {
      // Projected snapshot — copy only requested fields.
      projection->forEach([&](bison::key_t k, const bison::field&) {
        if (k == bison::dynamic::CLASS || k == bison::dynamic::PARENT) return;
        auto* f = obj.findField(k);
        if (f) result[k] = *f;
      });
    }

    // Apply __getter hook if present.
    if (obj.findMethod(HOOK_GETTER) != nullptr) {
      try { result = obj.call(HOOK_GETTER, result); }
      catch (...) {}
    }

    send_response(conn, env, OP_GET, shared::encode_payload(result));
  }

  static void handle_call(server::context&      ctx,
                           const bison::dynamic& env,
                           connection_iface&      conn) {
    using namespace shared::constants;

    std::string oid = env[FIELD_OBJECT_ID];
    auto it = ctx.objects.find(oid);
    if (it == ctx.objects.end()) {
      send_error(conn, env, OP_CALL, ERR_OBJECT_NOT_FOUND, "Object not found");
      return;
    }
    auto& obj = *it->second;

    const std::string payload_bytes = env[FIELD_PAYLOAD];
    auto              params        = shared::decode_payload(payload_bytes);

    // The method name is carried in FIELD_NAME inside the params payload.
    bison::key_t method_name = (*params)[FIELD_NAME];

    bool oneway = env[FIELD_ONEWAY];

    try {
      bison::dynamic call_result = obj.call(method_name, *params);
      if (!oneway) {
        send_response(conn, env, OP_CALL,
                      shared::encode_payload(call_result));
      }
    } catch (const std::exception& e) {
      if (!oneway) {
        send_error(conn, env, OP_CALL, ERR_INTERNAL_ERROR, e.what());
      }
    }
  }

  static void handle_destroy(server::context&      ctx,
                              const bison::dynamic& env,
                              connection_iface&      conn) {
    using namespace shared::constants;

    std::string oid = env[FIELD_OBJECT_ID];
    auto it = ctx.objects.find(oid);
    if (it == ctx.objects.end()) {
      send_error(conn, env, OP_DESTROY, ERR_OBJECT_NOT_FOUND,
                 "Object not found");
      return;
    }

    // Invoke __destruct on best-effort basis.
    if (it->second->findMethod(HOOK_DESTRUCT) != nullptr) {
      try { it->second->call(HOOK_DESTRUCT, bison::dynamic{}); }
      catch (...) {}
    }

    ctx.objects.erase(it);
    send_response(conn, env, OP_DESTROY, {});
  }

  static void handle_disconnect(server::context&      ctx,
                                 const bison::dynamic& env,
                                 connection_iface&      conn) {
    using namespace shared::constants;
    cleanup_context(ctx, conn);
    // Mark the connection closed so the worker loop exits.
    conn.close();
  }

  static void cleanup_context(server::context& ctx, connection_iface& /*conn*/) {
    using namespace shared::constants;
    // Invoke __destruct for each remaining live object (best-effort).
    for (auto& [oid, obj] : ctx.objects) {
      if (obj && obj->findMethod(HOOK_DESTRUCT) != nullptr) {
        try { obj->call(HOOK_DESTRUCT, bison::dynamic{}); } catch (...) {}
      }
    }
    ctx.objects.clear();
  }

  // ── Members ───────────────────────────────────────────────────────────────

  std::unique_ptr<transport_iface> transport_;
  std::thread                      accept_thread_;
  std::atomic<bool>                running_{false};

  std::mutex               workers_mutex_;
  std::vector<std::thread> workers_;

  // Bring context into scope under the server namespace alias.
  using context = server::context;
};

} // namespace bdg::bison::rmi
