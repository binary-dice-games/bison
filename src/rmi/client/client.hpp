// MIT License © 2025 Binary Dice Games
#pragma once

#include "src/core/bison.hpp"
#include "src/rmi/client/remote_dynamic.hpp"
#include "src/rmi/shared/constants.hpp"
#include "src/rmi/shared/envelope.hpp"
#include "src/rmi/shared/ids.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bdg::bison::rmi {

/**
 * @brief RMI client.
 *
 * Connects to an RMI server over a pluggable transport, manages a single
 * worker thread for inbound frame dispatch, and exposes high-level operations
 * for instantiating and using remote `bison::dynamic` objects.
 *
 * ### Usage
 * ```cpp
 * auto transport = memory_server.connect();
 * client c{std::move(transport)};
 * c.connect(bison::dynamic{});
 * auto proxy = c.instantiate("MyClass"_key);
 * proxy.set({{"field"_key, int32_t{42}}});
 * c.destroy(std::move(proxy));
 * c.disconnect();
 * ```
 *
 * @tparam TTransport  Any type satisfying the client-transport concept.
 */
class client {
 public:
  // ── Construction ─────────────────────────────────────────────────────────

  template <typename TTransport>
  explicit client(TTransport&& transport)
      : transport_(std::make_unique<transport_wrapper<std::decay_t<TTransport>>>(
            std::forward<TTransport>(transport))) {}

  client(const client&)            = delete;
  client& operator=(const client&) = delete;
  client(client&&)                 = delete;
  client& operator=(client&&)      = delete;

  ~client() {
    if (running_.load()) {
      try { disconnect(); } catch (...) {}
    }
  }

  // ── Public API ────────────────────────────────────────────────────────────

  /**
   * @brief Connect to the server and start the worker thread.
   *
   * @param params  Transport-specific connection parameters (e.g. host/port).
   */
  void connect(bison::dynamic params = bison::dynamic{}) {
    using namespace shared::constants;
    shared::register_envelope();

    transport_->open(std::move(params));
    running_.store(true);
    worker_ = std::thread(&client::worker_loop, this);

    // Send connect request and wait for response.
    bison::dynamic payload;
    payload[FIELD_VERSION] = int32_t{PROTOCOL_VERSION};
    auto f = send_request(OP_CONNECT, {}, std::move(payload), false);
    f.get();  // blocks; throws rmi_error on server-side failure
  }

  /**
   * @brief Query registered classes from the server.
   *
   * @param klass  0 (default) returns all classes; non-zero returns metadata
   *               for the specific class.
   * @return `bison::dynamic` descriptor (array of class keys for all-classes
   *         request, single descriptor for named-class request).
   */
  bison::dynamic describe(bison::key_t klass = 0U) {
    using namespace shared::constants;
    bison::dynamic payload;
    payload[FIELD_KLASS] = klass;
    auto f = send_request(OP_DESCRIBE, {}, std::move(payload), false);
    return f.get();
  }

  /**
   * @brief Instantiate a server-side object of the given registered class.
   *
   * @param klass   Hashed class name (must be registered on the server via
   *                `bison::dynamic::addClass`).
   * @param params  Construction parameters forwarded to the optional
   *                `__construct` hook.
   * @return Owning proxy for the newly created remote object.
   */
  remote::dynamic instantiate(bison::key_t   klass,
                              bison::dynamic params = bison::dynamic{}) {
    using namespace shared::constants;
    bison::dynamic payload;
    payload[FIELD_KLASS]  = klass;
    payload[FIELD_PARAMS] = std::make_shared<bison::dynamic>(std::move(params));

    auto f       = send_request(OP_INSTANTIATE, {}, std::move(payload), false);
    auto result  = f.get();

    std::string oid = result[FIELD_OBJECT_ID];
    return remote::dynamic{this, std::move(oid)};
  }

  /**
   * @brief Release a remote object, invoking its `__destruct` hook if
   *        registered.
   *
   * Consumes the proxy so post-destroy use is impossible by construction.
   */
  void destroy(remote::dynamic&& proxy) {
    using namespace shared::constants;
    std::string oid = proxy.object_id();
    proxy.valid_     = false;
    proxy.client_    = nullptr;

    bison::dynamic payload;
    auto f = send_request(OP_DESTROY, oid, std::move(payload), false);
    f.get();
  }

  /**
   * @brief Gracefully disconnect from the server and join the worker thread.
   */
  void disconnect() {
    if (!running_.load()) return;
    using namespace shared::constants;

    bison::dynamic payload;
    try {
      auto f = send_request(OP_DISCONNECT, {}, std::move(payload), true);
      f.get();
    } catch (...) {}

    running_.store(false);
    transport_->shutdown();
    if (worker_.joinable()) worker_.join();

    fail_all_pending(shared::constants::ERR_TRANSPORT_ERROR,
                     "Client disconnected");
  }

  // ── Internal helpers (used by remote::dynamic) ───────────────────────────

  std::future<bison::dynamic> send_request(bison::key_t       op,
                                            const std::string& object_id,
                                            bison::dynamic     payload,
                                            bool               oneway) {
    using namespace shared;
    using namespace shared::constants;

    const std::string request_id    = generate_id();
    const std::string payload_bytes = encode_payload(payload);
    auto              frame = encode_envelope(KIND_REQUEST, op, request_id,
                                              object_id, oneway, payload_bytes);

    if (oneway) {
      {
        std::lock_guard<std::mutex> lk(send_mutex_);
        transport_->send(std::move(frame));
      }
      std::promise<bison::dynamic> p;
      p.set_value(bison::dynamic{});
      return p.get_future();
    }

    std::promise<bison::dynamic> promise;
    auto future = promise.get_future();

    {
      std::lock_guard<std::mutex> lk(pending_mutex_);
      pending_[request_id] = std::move(promise);
    }

    try {
      std::lock_guard<std::mutex> lk(send_mutex_);
      transport_->send(std::move(frame));
    } catch (...) {
      // Retrieve and reject the promise, then rethrow.
      std::lock_guard<std::mutex> lk(pending_mutex_);
      auto it = pending_.find(request_id);
      if (it != pending_.end()) {
        it->second.set_exception(std::current_exception());
        pending_.erase(it);
      }
      throw;
    }

    return future;
  }

  void register_event_handler(const std::string&                   object_id,
                               bison::key_t                         name,
                               std::function<void(bison::dynamic)>  handler) {
    std::lock_guard<std::mutex> lk(event_mutex_);
    event_handlers_[object_id][name.id] = std::move(handler);
  }

  void unregister_object_events(const std::string& object_id) {
    std::lock_guard<std::mutex> lk(event_mutex_);
    event_handlers_.erase(object_id);
  }

 private:
  // ── Type-erased transport ─────────────────────────────────────────────────

  struct transport_iface {
    virtual void open(bison::dynamic params)                               = 0;
    virtual void send(std::vector<char> frame)                             = 0;
    virtual bool receive(std::vector<char>&,
                         std::chrono::milliseconds timeout)                = 0;
    virtual void shutdown()                                                = 0;
    virtual ~transport_iface()                                             = default;
  };

  template <typename T>
  struct transport_wrapper final : transport_iface {
    explicit transport_wrapper(T&& t) : t_(std::move(t)) {}
    void open(bison::dynamic p) override     { t_.open(std::move(p)); }
    void send(std::vector<char> f) override  { t_.send(std::move(f)); }
    bool receive(std::vector<char>& f,
                 std::chrono::milliseconds to) override {
      return t_.receive(f, to);
    }
    void shutdown() override { t_.shutdown(); }
    T t_;
  };

  // ── Worker thread ─────────────────────────────────────────────────────────

  void worker_loop() {
    while (running_.load(std::memory_order_acquire)) {
      std::vector<char> frame;
      if (!transport_->receive(frame, std::chrono::milliseconds{50})) {
        continue;
      }
      try {
        auto env = shared::decode_envelope(frame);
        process_frame(*env);
      } catch (const std::exception& /*e*/) {
        // Malformed frame — skip silently.
      }
    }
    fail_all_pending(shared::constants::ERR_TRANSPORT_ERROR,
                     "Worker thread exiting");
  }

  void process_frame(const bison::dynamic& env) {
    using namespace shared::constants;

    bison::key_t kind = env.as<bison::key_t>(FIELD_KIND);

    if (kind == KIND_RESPONSE) {
      std::string request_id = env.as<std::string>(FIELD_REQUEST_ID);

      std::promise<bison::dynamic> promise;
      {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        auto it = pending_.find(request_id);
        if (it == pending_.end()) return;
        promise = std::move(it->second);
        pending_.erase(it);
      }

      const std::string error_bytes = env.as<std::string>(FIELD_ERROR);
      if (!error_bytes.empty()) {
        auto error_obj = shared::decode_payload(error_bytes);
        bison::key_t code    = error_obj->as<bison::key_t>(FIELD_ERROR_CODE);
        std::string  message = error_obj->as<std::string>(FIELD_ERROR_MESSAGE);
        promise.set_exception(std::make_exception_ptr(
            shared::rmi_error{code, std::move(message)}));
      } else {
        const std::string payload_bytes = env.as<std::string>(FIELD_PAYLOAD);
        auto result = shared::decode_payload(payload_bytes);
        promise.set_value(std::move(*result));
      }

    } else if (kind == KIND_EVENT) {
      std::string  object_id    = env.as<std::string>(FIELD_OBJECT_ID);
      std::string  payload_bytes = env.as<std::string>(FIELD_PAYLOAD);
      auto         payload      = shared::decode_payload(payload_bytes);

      bison::key_t event_name = payload->as<bison::key_t>(FIELD_NAME);

      // __params is a nested dynamic stored as shared_ptr<dynamic>.
      bison::dynamic params;
      auto& params_field = (*payload)[FIELD_PARAMS];
      if (params_field.is<std::shared_ptr<bison::dynamic>>()) {
        auto ptr =
            static_cast<std::shared_ptr<bison::dynamic>>(params_field);
        if (ptr) params = std::move(*ptr);
      }

      std::function<void(bison::dynamic)> handler;
      {
        std::lock_guard<std::mutex> lk(event_mutex_);
        auto oit = event_handlers_.find(object_id);
        if (oit != event_handlers_.end()) {
          auto eit = oit->second.find(event_name.id);
          if (eit != oit->second.end()) handler = eit->second;
        }
      }

      if (handler) {
        try {
          handler(std::move(params));
        } catch (...) {
          // Handler exceptions must not crash the worker loop.
        }
      }
    }
  }

  void fail_all_pending(bison::key_t code, const std::string& message) {
    std::unordered_map<std::string, std::promise<bison::dynamic>> local;
    {
      std::lock_guard<std::mutex> lk(pending_mutex_);
      local = std::move(pending_);
    }
    for (auto& [id, promise] : local) {
      promise.set_exception(std::make_exception_ptr(
          shared::rmi_error{code, message}));
    }
  }

  // ── Members ───────────────────────────────────────────────────────────────

  std::unique_ptr<transport_iface> transport_;
  std::thread                      worker_;
  std::atomic<bool>                running_{false};

  std::mutex pending_mutex_;
  std::unordered_map<std::string, std::promise<bison::dynamic>> pending_;

  std::mutex event_mutex_;
  // object_id → (event_name hash → handler)
  std::unordered_map<std::string,
                     std::unordered_map<bison::hash_t,
                                        std::function<void(bison::dynamic)>>>
      event_handlers_;

  std::mutex send_mutex_;
};

// ─── remote::dynamic inline method bodies ────────────────────────────────────
// These depend on `client` being fully defined, so they are placed here.

namespace remote {

inline void dynamic::clear() {
  auto f = client_->send_request(
      shared::constants::OP_CLEAR, object_id_, bison::dynamic{}, false);
  f.get();
}

inline void dynamic::set(bison::dynamic fields) {
  auto f = client_->send_request(
      shared::constants::OP_SET, object_id_, std::move(fields), false);
  f.get();
}

inline void dynamic::get(bison::dynamic& fields) {
  bison::dynamic projection = fields;
  auto f = client_->send_request(
      shared::constants::OP_GET, object_id_, std::move(projection), false);
  fields = f.get();
}

inline std::future<bison::dynamic> dynamic::call(bison::dynamic params,
                                                   bool           oneway) {
  return client_->send_request(
      shared::constants::OP_CALL, object_id_, std::move(params), oneway);
}

inline void dynamic::onEvent(bison::key_t                             name,
                              std::function<void(bison::dynamic)> handler) {
  client_->register_event_handler(object_id_, name, std::move(handler));
}

} // namespace remote
} // namespace bdg::bison::rmi
