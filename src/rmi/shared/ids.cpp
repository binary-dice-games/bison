// MIT License © 2025 Binary Dice Games
/**
 * @file ids.cpp
 * @brief Implementation of opaque ID generation helpers used by RMI.
 */
#include "src/rmi/shared/ids.hpp"

#include <atomic>
#include <cstdint>

namespace bdg::bison::rmi::shared {

/**
 * @brief Generate a process-local opaque ID token using an atomic counter.
 */
bison::key_t generate_id() {
  static std::atomic<bison::hash_t> next{0x80000001u};
  return bison::key_t{next.fetch_add(1u, std::memory_order_relaxed)};
}

} // namespace bdg::bison::rmi::shared
