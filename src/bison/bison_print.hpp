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
 *   2. Entry in the caller-supplied `dict` (hash → display-name map).
 *   3. Raw hash (`#xxxxxxxx`) or array index (`[n]`).
 *
 * The companion free function `build_display_dict()` populates a dictionary
 * from the global class registry so callers can pass it to `print_options::dict`
 * without repeating the traversal logic.
 *
 * ## Usage
 * @code{.cpp}
 * #include "src/bison/bison_print.hpp"
 *
 * // Simple print (uses DisplayName attributes only).
 * std::cout << bison::print(obj) << '\n';
 *
 * // Print with registry-based name resolution.
 * auto dict = bison::build_display_dict();
 * bison::print_options opts;
 * opts.dict = &dict;
 * std::cout << bison::print(obj, opts) << '\n';
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
   * @brief Optional hash→display-name dictionary used as a fallback when a
   *        field or method key has no `DisplayName` attribute.
   *
   * Build one with `build_display_dict()`.  Null (the default) means no
   * dictionary lookup is performed and unknown hashes are printed raw.
   */
  const std::unordered_map<hash_t, std::string>* dict{nullptr};
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
 * @param opts  Format options; defaults to multiline with two-space indent and
 *              no dictionary lookup.
 * @return Formatted string representation of @p obj.
 */
std::string print(const dynamic& obj, const print_options& opts = {});

/**
 * @brief Build a hash→display-name dictionary from all registered classes.
 *
 * Traverses the global class registry and collects `DisplayName` attributes
 * from class keys, field keys, method keys, and method parameter field keys.
 * Pass the result (or a pointer to it) to `print_options::dict` so that
 * `print()` can resolve unknown hashes to human-readable names.
 *
 * @return Dictionary mapping `hash_t` values to their display names.
 */
std::unordered_map<hash_t, std::string> build_display_dict();

} // namespace bdg::bison
