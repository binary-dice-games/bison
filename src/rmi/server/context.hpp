// MIT License © 2025 Binary Dice Games
/**
 * @file context.hpp
 * @brief Per-connection runtime state for server-side RMI request handling.
 */
#pragma once

#include "src/core/bison.hpp"
#include "src/rmi/shared/ids.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace bdg::bison::rmi {

/**
 * @brief Mutable context owned by a server worker for one client session.
 */
struct context {
  /** @brief Unique session identifier for the connected client. */
  std::string session_id;

  /** @brief Live object table keyed by remote object identifier string. */
  std::unordered_map<std::string, std::shared_ptr<bison::dynamic>> objects;

  /**
   * @brief Callback used by server logic to emit asynchronous events.
   *
   * Parameters are `(object_id, event_name, payload)`.
   */
  std::function<void(const std::string&, bison::key_t, bison::dynamic)> emit_event;
};

} // namespace bdg::bison::rmi
