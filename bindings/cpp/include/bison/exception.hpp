// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file exception.hpp
 * @brief Exception types that translate `bison_c.h` / `rmi_c.h` error codes
 *        into C++ exceptions.
 *
 * The internal C++ API (`src/bison/bison_object.hpp`) reports failures by
 * throwing `std::runtime_error`. The C ABI cannot throw across the DLL
 * boundary, so it reports failures as negative return codes instead (see
 * `bison_error` / `rmi_error` in `bison_c.h` / `rmi_c.h`). This header
 * restores exception-based error handling on top of the ABI -- every
 * throwing call in this binding wraps its `bison_*`/`rmi_*` call and raises
 * one of these on a non-`BISON_OK`/`RMI_OK` result, so application code
 * written against this binding looks like application code written against
 * the internal API (try/catch, not manual error-code checks).
 */

#pragma once

#include "bison_c.h"
#include "rmi_c.h"

#include <stdexcept>
#include <string>

namespace bdg::bison::abi {

/**
 * @brief Thrown when a `bison_*` C ABI call returns a non-`BISON_OK` code.
 *
 * Named `bison_exception` rather than `bison_error` (matching the
 * `rmi_exception` / `rmi_error` naming below) so that a consumer who writes
 * `using namespace bdg::bison::abi;` never gets an ambiguous-lookup error
 * between this class and the C ABI's own `::bison_error` enum (`bison_c.h`)
 * -- `catch (const bison_error&)` would otherwise be genuinely ambiguous
 * between the two same-named, differently-scoped entities.
 */
class bison_exception : public std::runtime_error {
 public:
  bison_exception(::bison_error code, const std::string& context)
      : std::runtime_error(context + ": " + message_for(code)), code(code) {}

  /** @brief The underlying `bison_error` code returned by the C ABI call. */
  ::bison_error code;

  static const char* message_for(::bison_error code) {
    switch (code) {
      case BISON_OK:
        return "success";
      case BISON_ERR_NULL:
        return "null handle or pointer";
      case BISON_ERR_TYPE:
        return "field type mismatch";
      case BISON_ERR_NOT_FOUND:
        return "method or field not found";
      case BISON_ERR_DUPLICATE:
        return "duplicate class or method";
      case BISON_ERR_EXCEPTION:
        return "internal C++ exception";
      case BISON_ERR_PARSE:
        return "parse error (JSON / YAML)";
      default:
        return "unknown bison_error";
    }
  }
};

/**
 * @brief Thrown when an `rmi_*` C ABI call returns a non-`RMI_OK` code.
 */
class rmi_exception : public std::runtime_error {
 public:
  rmi_exception(::rmi_error code, const std::string& context)
      : std::runtime_error(context + ": " + message_for(code)), code(code) {}

  /** @brief The underlying `rmi_error` code returned by the C ABI call. */
  ::rmi_error code;

  static const char* message_for(::rmi_error code) {
    switch (code) {
      case RMI_OK:
        return "success";
      case RMI_ERR_NULL:
        return "null handle or pointer";
      case RMI_ERR_INVALID_STATE:
        return "operation invalid for current state";
      case RMI_ERR_TIMEOUT:
        return "request timed out";
      case RMI_ERR_REMOTE_EXCEPTION:
        return "server raised an exception";
      case RMI_ERR_TRANSPORT:
        return "transport error";
      case RMI_ERR_EXCEPTION:
        return "internal C++ exception";
      default:
        return "unknown rmi_error";
    }
  }
};

namespace detail {

/** @brief Throw `bison_exception` if @p rc is not `BISON_OK`. */
inline void check(::bison_error rc, const std::string& context) {
  if (rc != BISON_OK)
    throw bison_exception(rc, context);
}

/** @brief Throw `rmi_exception` if @p rc is not `RMI_OK`. */
inline void check(::rmi_error rc, const std::string& context) {
  if (rc != RMI_OK)
    throw rmi_exception(rc, context);
}

} // namespace detail
} // namespace bdg::bison::abi
