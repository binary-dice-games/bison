// MIT License © 2025 Binary Dice Games
#pragma once

#include "src/core/bison.hpp"
#include "src/rmi/shared/ids.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace bdg::bison::rmi::server {

/**
 * @brief Per-client session state held by the server worker thread.
 *
 * Each accepted client connection receives an exclusive `context`.  Object
 * ids are scoped to the session so client A cannot reference objects created
 * by client B.
 */
struct context {
  /// Opaque random session identifier.
  std::string session_id{shared::generate_id()};

  /// Live remote objects indexed by their opaque string id.
  std::unordered_map<std::string, std::shared_ptr<bison::dynamic>> objects;

  /**
   * @brief Callback that sends a server-initiated event frame to the client.
   *
   * Set by the server worker before processing begins.  Signature:
   *   `void(object_id, event_name, params)`
   */
  std::function<void(const std::string&, bison::key_t, bison::dynamic)>
      emit_event;
};

} // namespace bdg::bison::rmi::server
