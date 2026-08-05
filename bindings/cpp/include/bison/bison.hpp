// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file bison.hpp
 * @brief Umbrella include for the header-only C++ ABI binding.
 *
 * `#include "bison/bison.hpp"` and link `bison_abi` (see `docs/bindings.md`)
 * to get the full binding: `bdg::bison::abi::dynamic` /
 * `bdg::bison::abi::key_t` / `"name"_key` (dynamic-object API, wrapping
 * `bison_c.h`) plus `bdg::bison::abi::rmi::{client,server,proxy,future}`
 * (RMI API, wrapping `rmi_c.h`). Include `bison/dynamic.hpp` alone for
 * just the dynamic-object API without pulling in RMI.
 */

#pragma once

#include "attributes.hpp"
#include "dynamic.hpp"
#include "exception.hpp"
#include "key.hpp"
#include "rmi.hpp"
