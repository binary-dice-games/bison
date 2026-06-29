// MIT License © 2025 Binary Dice Games
/**
 * @file ids.cpp
 * @brief Implementation of opaque ID generation helpers used by RMI.
 */
#include "src/rmi/shared/ids.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace bdg::bison::rmi::shared {

namespace {

// Mixes a 32-bit value with good avalanche properties to obscure sequence
// structure while staying branch-free and lock-free.
inline bison::hash_t mix32(bison::hash_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

} // namespace

/**
 * @brief Generate a process-local opaque ID token without locks.
 */
bison::key_t generate_id() {
  constexpr bison::hash_t kGoldenStep = 0x9e3779b9u;
  static const bison::hash_t seed = []() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    bison::hash_t s = static_cast<bison::hash_t>(now);
    s ^= static_cast<bison::hash_t>(static_cast<uint64_t>(now) >> 32);
    s ^= 0x80000001u;
    return s;
  }();

  static std::atomic<bison::hash_t> state{seed};
  const bison::hash_t raw = state.fetch_add(kGoldenStep, std::memory_order_relaxed);
  return bison::key_t{mix32(raw)};
}

} // namespace bdg::bison::rmi::shared
