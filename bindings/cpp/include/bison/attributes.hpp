// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file attributes.hpp
 * @brief Optional display/documentation metadata for a class, field, or
 *        method, mirroring `bison_attributes` (`bison_c.h`).
 *
 * The internal C++ API attaches metadata via polymorphic `attribute`
 * subclasses (`DisplayName`, `Description`, `Category`, ...; see
 * `src/bison/bison_object.hpp`). The C ABI only exposes the fixed subset
 * that `bison_attributes` carries, so this binding follows the ABI's flat
 * struct shape rather than the polymorphic hierarchy -- there is no ABI
 * entry point to attach an arbitrary custom `attribute` subclass from
 * outside the shared library.
 */

#pragma once

#include "bison_c.h"

#include <optional>
#include <string>

namespace bdg::bison::abi {

/**
 * @brief Optional display/documentation metadata for a class, field, or
 *        method. C++ analogue of `bison_attributes`.
 */
struct attributes {
  std::optional<std::string> display_name;
  std::optional<std::string> description;
  std::optional<std::string> category;
  bool obsolete = false;
  std::optional<std::string> obsolete_message;
  bool required = false;

  /** @brief True when every field is at its default (no metadata set). */
  bool empty() const {
    return !display_name && !description && !category && !obsolete && !obsolete_message && !required;
  }

  /** @brief Convert to the C ABI's `bison_attributes` struct. Kept simple:
   *         the returned struct borrows `const char*` pointers into `this`,
   *         so it must not outlive `this`. */
  ::bison_attributes to_c() const {
    ::bison_attributes c{};
    c.display_name = display_name ? display_name->c_str() : nullptr;
    c.description = description ? description->c_str() : nullptr;
    c.category = category ? category->c_str() : nullptr;
    c.obsolete = obsolete ? 1 : 0;
    c.obsolete_message = obsolete_message ? obsolete_message->c_str() : nullptr;
    c.required = required ? 1 : 0;
    return c;
  }

  /** @brief Build from the C ABI's `bison_attributes` struct. */
  static attributes from_c(const ::bison_attributes& c) {
    attributes a;
    if (c.display_name)
      a.display_name = c.display_name;
    if (c.description)
      a.description = c.description;
    if (c.category)
      a.category = c.category;
    a.obsolete = c.obsolete != 0;
    if (c.obsolete_message)
      a.obsolete_message = c.obsolete_message;
    a.required = c.required != 0;
    return a;
  }
};

} // namespace bdg::bison::abi
