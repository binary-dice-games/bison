// MIT License © 2025 Binary Dice Games
/**
 * @file envelope.hpp
 * @brief RMI envelope frame model and wire codec.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/shared/constants.hpp"

namespace bdg::bison::rmi::shared {

/**
 * @brief RMI envelope frame model and codec.
 *
 * Carries a versioned protocol frame over the transport layer.
 */
struct envelope {
  int32_t version{constants::PROTOCOL_VERSION};
  bison::key_t kind{0u};
  bison::key_t op{0u};
  bison::key_t request_id{0u};
  bison::key_t object_id{0u};
  /**
   * @brief Group newly-created objects should be filed under while this
   * request is handled (see `context::current_group`); `0` means no group.
   * Also carries the target group for `OP_DESTROY_GROUP` requests.
   */
  bison::key_t group{0u};
  bison::dynamic payload{};
  bison::dynamic error{};
  bool with_schema{false};
  bool oneway{false};

  envelope() = default;

  /** @brief Encode this envelope to a transport frame. */
  bison::buffer encode() const;

  /** @brief Decode a transport frame into an envelope object. */
  static envelope decode(const bison::buffer& bytes);
};

} // namespace bdg::bison::rmi::shared
