// MIT License © 2025 Binary Dice Games
/**
 * @file envelope.hpp
 * @brief RMI envelope frame model and wire codec.
 */
#pragma once

#include "src/core/bison.hpp"
#include "src/rmi/shared/constants.hpp"
#include "src/rmi/shared/error.hpp"
#include "src/rmi/shared/payload.hpp"
#include "src/rmi/shared/schema_registry.hpp"

namespace bdg::bison::rmi::shared {

/**
 * @brief RMI envelope frame model and codec.
 *
 * Carries a versioned protocol frame over the transport layer.
 * The `payload` and `error` fields hold encoded blobs produced by
 * the corresponding codec structs; their raw bytes are preserved
 * across encode/decode round-trips.
 */
struct envelope {
  int32_t version{constants::PROTOCOL_VERSION};
  bison::key_t kind{0u};
  bison::key_t op{0u};
  bison::key_t request_id{0u};
  bison::key_t object_id{0u};
  bool oneway{false};
  bison::buffer payload{};
  bison::buffer error{};

  envelope() = default;

  /**
   * @brief Construct a request/event envelope with a payload.
   * @param kind       Message kind token.
   * @param op         Operation token.
   * @param request_id Correlation identifier.
   * @param object_id  Target server-side object.
   * @param oneway     True if no response is expected.
   * @param pl         Payload to encode into the frame.
   */
  envelope(
      bison::key_t kind,
      bison::key_t op,
      bison::key_t request_id,
      bison::key_t object_id,
      bool oneway,
      ::bdg::bison::rmi::shared::payload&& pl);

  /**
   * @brief Construct a response envelope with both payload and error.
   * @param kind       Message kind token.
   * @param op         Operation token.
   * @param request_id Correlation identifier.
   * @param object_id  Target server-side object.
   * @param oneway     True if no response is expected.
   * @param pl         Payload to encode into the frame.
   * @param err        Error to encode into the frame.
   */
  envelope(
      bison::key_t kind,
      bison::key_t op,
      bison::key_t request_id,
      bison::key_t object_id,
      bool oneway,
      ::bdg::bison::rmi::shared::payload&& pl,
      ::bdg::bison::rmi::shared::error&& err);

  /** @brief Encode this envelope to a transport frame. */
  bison::buffer encode() const;

  /** @brief Decode a transport frame into an envelope object. */
  static envelope decode(const bison::buffer& bytes);

  /** @brief Register envelope-related schemas in the global class registry. */
  static void register_schemas();
};

} // namespace bdg::bison::rmi::shared
