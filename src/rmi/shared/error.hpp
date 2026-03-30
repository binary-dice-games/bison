// MIT License © 2025 Binary Dice Games
/**
 * @file error.hpp
 * @brief Protocol-level RMI error type.
 */
#pragma once

#include "src/core/bison.hpp"

#include <stdexcept>
#include <string>

namespace bdg::bison::rmi::shared {

/**
 * @brief Protocol-level exception containing an RMI error code.
 *
 * Inherits from `std::runtime_error` so it can be thrown and caught
 * as a standard exception. The canonical error code is stored as a
 * public field alongside the human-readable message in the base class.
 */
struct error : std::runtime_error {
  /** @brief Canonical error code token. */
  bison::key_t code{0u};

  /**
   * @brief Construct an RMI error.
   * @param code  Canonical error code token.
   * @param message Human-readable error message.
   */
  error(bison::key_t code, std::string message);

  /** @brief Serialize this error as payload bytes. */
  bison::buffer encode() const;

  /** @brief Decode a serialized error payload. */
  static error decode(const bison::buffer& bytes);
};

} // namespace bdg::bison::rmi::shared
