// MIT License © 2025 Binary Dice Games
/**
 * @file ids.hpp
 * @brief ID generation utilities for RMI sessions and objects.
 */
#pragma once

#include "src/core/bison.hpp"

namespace bdg::bison::rmi::shared {

/**
 * @brief Generate a lock-free opaque identifier token.
 * @return Newly generated identifier.
 */
bison::key_t generate_id();
} // namespace bdg::bison::rmi::shared
