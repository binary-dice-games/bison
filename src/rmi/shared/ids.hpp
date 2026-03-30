// MIT License © 2025 Binary Dice Games
/**
 * @file ids.hpp
 * @brief ID generation utilities for RMI sessions and objects.
 */
#pragma once
#include <string>

namespace bdg::bison::rmi::shared {

/**
 * @brief Generate a protocol-safe opaque identifier string.
 * @return Newly generated identifier.
 */
std::string generate_id();
} // namespace bdg::bison::rmi::shared
