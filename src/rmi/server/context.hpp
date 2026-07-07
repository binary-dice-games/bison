// MIT License © 2025 Binary Dice Games
/**
 * @file context.hpp
 * @brief Per-connection runtime state for server-side RMI request handling.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/shared/ids.hpp"

#include <functional>
#include <memory>
#include <unordered_map>

namespace bdg::bison::rmi {

/**
 * @brief Mutable context owned by a server worker for one client session.
 *
 * Polymorphic: subclasses may attach application-specific per-session state.
 * See `server::on_create_context` for the extension point.
 */
struct context {
  context() = default;

  /** @brief Construct with a known session id. */
  explicit context(bison::key_t session_id) : session_id(session_id) {}

  virtual ~context() = default;

  /** @brief Unique session identifier for the connected client. */
  bison::key_t session_id;

  /** @brief Live object table keyed by remote object identifier token. */
  std::unordered_map<bison::hash_t, bison::dynamic_ptr> objects;

  /**
   * @brief Callback used by server logic to emit asynchronous events.
   *
   * Parameters are `(object_id, event_name, payload)`.
   */
  std::function<void(bison::key_t, bison::key_t, bison::dynamic)> emit_event;
};

} // namespace bdg::bison::rmi
