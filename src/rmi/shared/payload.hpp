// MIT License © 2025 Binary Dice Games
/**
 * @file payload.hpp
 * @brief Serialized dynamic payload codec.
 */
#pragma once

#include "src/core/bison.hpp"

namespace bdg::bison::rmi::shared {

/**
 * @brief Serialized dynamic payload codec.
 *
 * Holds a `bison::dynamic` value and provides round-trip encode/decode
 * against the binary wire format.
 */
struct payload {
  /** @brief The decoded or constructed payload value. */
  bison::dynamic value{};

  payload() = default;

  /**
   * @brief Construct from a dynamic value.
   * @param val Value to wrap.
   */
  explicit payload(bison::dynamic val);

  /** @brief Serialize the payload value to bytes. */
  bison::buffer encode() const;

  /** @brief Decode payload bytes into a payload object. */
  static payload decode(const bison::buffer& bytes);
};

} // namespace bdg::bison::rmi::shared
