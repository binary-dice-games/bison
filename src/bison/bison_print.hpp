// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file bison_print.hpp
 * @brief Human-readable string formatting for `dynamic` objects.
 *
 * Provides `print_options` and the free function `print(dynamic, print_options)`
 * that converts a `dynamic` object to a readable string.  The output is
 * configurable between multiline (indented) and single-line (compact) modes.
 *
 * Field and method keys are resolved in this order:
 *   1. `DisplayName` attribute on the field or method.
 *   2. The live `_rkey` / `register_key_name` registry, consulted via
 *      `lookup_registered_key_name()` (see `bison_common.hpp`).
 *   3. Raw hash (`#xxxxxxxx`) or array index (`[n]`).
 *
 * Step 2 reads the registry directly on every lookup, so a key registered
 * (via `_rkey`) after an object was built -- or after an earlier `print()`
 * call -- still resolves correctly; there is no dictionary to build or
 * cache.
 *
 * ## Usage
 * @code{.cpp}
 * #include "src/bison/bison_print.hpp"
 *
 * std::cout << bison::print(obj) << '\n';
 * @endcode
 */

#pragma once

#include "src/bison/bison_object.hpp"

namespace bdg::bison {

/**
 * @brief Options controlling the output format of `print()`.
 */
struct print_options {
  /** @brief Emit one key-value pair per line with indentation when true. */
  bool multiline{true};
  /** @brief String prepended once per nesting level in multiline mode. */
  std::string indent{"  "};
  /**
   * @brief When `true`, suppress the internal `__class`, `__parent`, and
   *        `__namespace` fields from the output.
   *
   * These fields are bookkeeping metadata stored on every `dynamic` object.
   * They clutter human-readable traces; set this flag to hide them.
   * Default: `false` (show all fields).
   */
  bool hide_internal{false};
};

/**
 * @brief Convert @p obj to a human-readable string.
 *
 * @param obj   Source object.
 * @param opts  Format options; defaults to multiline with two-space indent.
 * @return Formatted string representation of @p obj.
 */
std::string print(const dynamic& obj, const print_options& opts = {});

} // namespace bdg::bison
