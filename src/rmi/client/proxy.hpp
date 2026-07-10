// MIT License © 2025 Binary Dice Games
/**
 * @file proxy.hpp
 * @brief Remote object proxy API used by the RMI client and standalone.
 */
#pragma once

#include "src/bison/bison.hpp"

#include <cstdint>
#include <functional>
#include <future>

namespace bdg::bison::rmi {

// Forward declarations — defined in client.hpp and standalone.hpp respectively.
class client;
class standalone;

/**
 * @brief Abstract backend interface used by `proxy::dynamic` to dispatch
 *        operations without being coupled to a specific transport type.
 *
 * Both `client` (transport-backed) and `standalone` (in-process) inherit
 * from this interface so that `proxy::dynamic` can remain transport-agnostic.
 * The two virtual methods cover the complete set of operations a proxy needs.
 *
 * @par Thread safety
 * Implementers are responsible for documenting their own thread-safety
 * guarantees.  `client` is thread-safe (guards its state with mutexes);
 * `standalone` is also thread-safe for concurrent proxy operations
 * (all requests are serialized through a background worker thread).
 */
struct proxy_backend {
  /**
   * @brief Send a protocol operation to the backend and return a future result.
   *
   * @param op        Operation token (e.g. `OP_CALL`, `OP_GET`).
   * @param object_id Target object identifier.
   * @param payload   Operation payload (consumed by move).
   * @param oneway    When true, no response is expected.
   * @return Future resolved with the response payload.
   */
  virtual std::future<bison::dynamic>
  send_request(bison::key_t op, bison::key_t object_id, bison::dynamic payload, bool oneway) = 0;

  /**
   * @brief Register an event handler for server-initiated events.
   *
   * @param object_id  Object that emits the event.
   * @param name       Event name token.
   * @param handler    Callback invoked with the event payload.
   */
  virtual void
  register_event_handler(bison::key_t object_id, bison::key_t name, std::function<void(bison::dynamic)> handler) = 0;

  /**
   * @brief Tear down bookkeeping for a remote/in-process object: unregister
   * its event handlers and destroy it (sending `OP_DESTROY` for a networked
   * `client`; direct in-process teardown for `standalone`).
   *
   * This is the single, backend-agnostic entry point `proxy::dynamic::destroy()`
   * calls -- `client::destroy(proxy::dynamic&&)` / `standalone::destroy(proxy::dynamic&&)`
   * delegate to it too, so every teardown path (a C++ caller moving its proxy
   * into one of those, or a caller like the C ABI's `rmi_proxy_release()`
   * that only has a `proxy::dynamic*` and no access to the concrete backend
   * type) ends up unregistering event handlers the same way. Without this,
   * a released/destroyed proxy's event registration outlives it, and a
   * later server-sent event for the same object ID finds a "live-looking"
   * but possibly-dangling handler instead of simply finding none.
   *
   * @param object_id  Object to destroy.
   */
  virtual void destroy_object(bison::key_t object_id) = 0;

  virtual ~proxy_backend() = default;
};

namespace proxy {

/**
 * @brief Move-only owning proxy for a server-side or in-process
 *        `bison::dynamic` object.
 *
 * A `proxy::dynamic` is created exclusively by `client::instantiate` or
 * `standalone::instantiate` and released by the corresponding `destroy` call.
 * It forwards every operation through the `proxy_backend` interface so that
 * the same proxy type works transparently over a network transport or in
 * a standalone in-process session.
 *
 * Ownership rules:
 * - Non-copyable; moveable.
 * - A moved-from proxy is invalid (checked by `valid()`).
 * - Exactly one live proxy per remote object prevents double-destroy.
 */
class dynamic {
  friend class bdg::bison::rmi::client;
  friend class bdg::bison::rmi::standalone;

 public:
  dynamic(const dynamic&) = delete;
  dynamic& operator=(const dynamic&) = delete;

  dynamic(dynamic&& other) noexcept
      : backend_(other.backend_), object_id_(std::move(other.object_id_)), valid_(other.valid_) {
    other.valid_ = false;
    other.backend_ = nullptr;
  }

  dynamic& operator=(dynamic&& other) noexcept {
    if (this != &other) {
      backend_ = other.backend_;
      object_id_ = std::move(other.object_id_);
      valid_ = other.valid_;
      other.valid_ = false;
      other.backend_ = nullptr;
    }
    return *this;
  }

  ~dynamic() = default;

  // ── Remote operations
  // ───────────────────────────────────────────────────────

  /**
   * @brief Clear explicitly set fields on the remote object, reverting it to
   *        prototype / inherited defaults.
   * @return Future resolved with true on success, or with an exception on
   * failure.
   */
  std::future<bool> clear();

  /**
   * @brief Apply a partial field update to the remote object without resetting
   *        unspecified fields.
   * @return Future resolved with true on success, or with an exception on
   * failure.
   */
  std::future<bool> set(bison::dynamic fields);

  /**
   * @brief Retrieve a full snapshot of all fields from the remote object.
   * @return Future resolved with the complete field snapshot.
   */
  std::future<bison::dynamic> get();

  /**
   * @brief Retrieve a projected subset of fields from the remote object.
   *
   * Only the members present in @p projection are filled in the response
   * (GraphQL-style). @p projection is consumed by move.
   * @param projection Projection shape describing the desired fields.
   * @return Future resolved with the projected field snapshot.
   */
  std::future<bison::dynamic> get(bison::dynamic&& projection);

  /**
   * @brief Invoke a callable behavior on the remote object.
   *
   * @param name    Name token of the method to invoke.
   * @param params  Call arguments (consumed by move).
   * @param oneway  When true the server does not send a response and the
   *                returned future resolves immediately with an empty result.
   * @return Future that resolves with the call result (or empty if oneway).
   */
  std::future<bison::dynamic> call(bison::key_t name, bison::dynamic&& params, bool oneway = false);

  /**
   * @brief Register a handler for a named server-initiated event.
   *
   * Handlers are dispatched serially on the client worker thread or in-process,
   * guaranteeing ordering.  Exceptions thrown by the handler are caught and
   * silently discarded.
   *
   * @param name     Hashed event name token.
   * @param handler  Callable invoked with the event params dynamic.
   */
  void onEvent(bison::key_t name, std::function<void(bison::dynamic)> handler);

  /**
   * @brief Destroy the remote/in-process object through the backend
   * (unregistering its event handlers as part of the same call -- see
   * `proxy_backend::destroy_object()`), then invalidate this proxy.
   *
   * Equivalent to `client::destroy(std::move(*this))` /
   * `standalone::destroy(std::move(*this))` for callers that only have a
   * `proxy::dynamic&`/`*` and not the originating `client`/`standalone`
   * (e.g. the C ABI's `rmi_proxy_release()`). A no-op if already invalid.
   */
  void destroy();

  // ── Accessors
  // ───────────────────────────────────────────────────────────────

  /**
   * @brief Return the opaque object identifier token as `uint64_t`.
   */
  uint64_t id() const {
    return static_cast<uint64_t>(object_id_.id);
  }

  /** @brief True when this proxy refers to a live remote object. */
  bool valid() const {
    return valid_;
  }

  /** @brief Raw opaque object identifier token. */
  bison::key_t object_id() const {
    return object_id_;
  }

 private:
  // Only client and standalone can construct proxies.
  dynamic(proxy_backend* backend, bison::key_t id) : backend_(backend), object_id_(std::move(id)), valid_(true) {}

  proxy_backend* backend_{nullptr};
  bison::key_t object_id_;
  bool valid_{false};
};

} // namespace proxy
} // namespace bdg::bison::rmi
