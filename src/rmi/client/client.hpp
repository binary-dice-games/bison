// MIT License © 2025 Binary Dice Games
/**
 * @file client.hpp
 * @brief RMI client runtime for request/response and event dispatch.
 */
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
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bdg::bison::rmi {

/**
 * @brief Owns a transport connection and performs RMI protocol operations.
 *
 * The client serializes requests into RMI envelopes, receives responses on a
 * background worker thread, resolves pending futures by request ID, and routes
 * server events to user-registered handlers.
 *
 * @tparam TTransport Transport type accepted by the constructor. It must
 *         provide `open`, `send`, `receive`, and `shutdown` methods with
 *         signatures compatible with the internal type-erased wrapper.
 */
class client {
 public:
  template <typename TTransport>
  explicit client(TTransport&& transport)
      : transport_(
            std::make_unique<transport_wrapper<std::decay_t<TTransport>>>(
                std::forward<TTransport>(transport))) {}

  client(const client&) = delete;
  client& operator=(const client&) = delete;
  client(client&&) = delete;
  client& operator=(client&&) = delete;

  ~client();

  /**
   * @brief Open the transport and start the client worker loop.
   * @param params Optional transport-specific startup parameters.
   */
  void connect(bison::dynamic params = bison::dynamic{});

  /**
   * @brief Request class metadata from the server.
   * @param klass Optional class key. Pass `0` to request full metadata.
   * @return Description payload returned by the server.
   */
  bison::dynamic describe(bison::key_t klass = 0U);

  /**
   * @brief Create a remote object instance on the server.
   * @param klass Class key to instantiate.
   * @param params Optional constructor parameters.
   * @return Move-only proxy that references the server-side object.
   */
  proxy::dynamic instantiate(
      bison::key_t klass,
      bison::dynamic params = bison::dynamic{});

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
  std::future<bison::dynamic> send_request(
      bison::key_t op,
      bison::key_t object_id,
      bison::dynamic payload,
      bool oneway);

  /**
   * @brief Register a handler for server-sent events on an object.
   * @param object_id Remote object identifier.
   * @param name Event name token.
   * @param handler Callback invoked with event payload.
   */
  void register_event_handler(
      bison::key_t object_id,
      bison::key_t name,
      std::function<void(bison::dynamic)> handler);

  /**
   * @brief Remove all event handlers associated with an object ID.
   * @param object_id Remote object identifier.
   */
  void unregister_object_events(bison::key_t object_id);

 private:
  // ── Type-erased transport ─────────────────────────────────────────────────

  struct transport_iface {
    virtual void open(bison::dynamic params) = 0;
    virtual void send(bison::buffer frame) = 0;
    virtual bool receive(bison::buffer&, std::chrono::milliseconds timeout) = 0;
    virtual void shutdown() = 0;
    virtual ~transport_iface() = default;
  };

  template <typename T>
  struct transport_wrapper final : transport_iface {
    explicit transport_wrapper(T&& t) : t_(std::move(t)) {}
    void open(bison::dynamic p) override {
      t_.open(std::move(p));
    }
    void send(bison::buffer f) override {
      t_.send(std::move(f));
    }
    bool receive(bison::buffer& f, std::chrono::milliseconds to) override {
      return t_.receive(f, to);
    }
    void shutdown() override {
      t_.shutdown();
    }
    T t_;
  };

  // ── Private methods (defined in client.cpp) ───────────────────────────────

  void worker_loop();
  void process_frame(const shared::envelope& env);
  void fail_all_pending(bison::key_t code, const std::string& message);

  // ── Members ───────────────────────────────────────────────────────────────

  std::unique_ptr<transport_iface> transport_;
  std::thread worker_;
  std::atomic<bool> running_{false};

  bison::synchronized<
      std::unordered_map<bison::hash_t, std::promise<bison::dynamic>>>
      pending_;

  bison::synchronized<std::unordered_map<
      bison::hash_t,
      std::unordered_map<bison::hash_t, std::function<void(bison::dynamic)>>>>
      event_handlers_;

  std::mutex send_mutex_;
};

} // namespace bdg::bison::rmi
