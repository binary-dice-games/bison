// MIT License © 2025 Binary Dice Games
/**
 * @file proxy.hpp
 * @brief Remote object proxy API used by the RMI client.
 */
#pragma once

#include "src/core/bison.hpp"

#include <cstdint>
#include <functional>
#include <future>

namespace bdg::bison::rmi {

// Forward declaration — defined in client.hpp.
class client;

namespace proxy {

/**
 * @brief Move-only owning proxy for a server-side `bison::dynamic` object.
 *
 * A `proxy::dynamic` is created exclusively by `client::instantiate` and
 * released by `client::destroy`.  It forwards every operation to the server
 * over the active transport connection.
 *
 * Ownership rules:
 * - Non-copyable; moveable.
 * - A moved-from proxy is invalid (checked by `valid()`).
 * - Exactly one live proxy per remote object prevents double-destroy.
 */
class dynamic {
  friend class bdg::bison::rmi::client;

 public:
  dynamic(const dynamic&) = delete;
  dynamic& operator=(const dynamic&) = delete;

  dynamic(dynamic&& other) noexcept
      : client_(other.client_),
        object_id_(std::move(other.object_id_)),
        valid_(other.valid_) {
    other.valid_ = false;
    other.client_ = nullptr;
  }

  dynamic& operator=(dynamic&& other) noexcept {
    if (this != &other) {
      client_ = other.client_;
      object_id_ = std::move(other.object_id_);
      valid_ = other.valid_;
      other.valid_ = false;
      other.client_ = nullptr;
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
  std::future<bison::dynamic>
  call(bison::key_t name, bison::dynamic&& params, bool oneway = false);

  /**
   * @brief Register a handler for a named server-initiated event.
   *
   * Handlers are dispatched serially on the client worker thread, guaranteeing
   * ordering.  Exceptions thrown by the handler are caught and silently
   * discarded to keep the worker loop alive.
   *
   * @param name     Hashed event name token.
   * @param handler  Callable invoked with the event params dynamic.
   */
  void onEvent(bison::key_t name, std::function<void(bison::dynamic)> handler);

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
  // Only client can construct proxies.
  dynamic(class bdg::bison::rmi::client* c, bison::key_t id)
      : client_(c), object_id_(std::move(id)), valid_(true) {}

  class bdg::bison::rmi::client* client_{nullptr};
  bison::key_t object_id_;
  bool valid_{false};
};

} // namespace proxy
} // namespace bdg::bison::rmi
