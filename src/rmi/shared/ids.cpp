// MIT License © 2025 Binary Dice Games
/**
 * @file ids.cpp
 * @brief Implementation of opaque ID generation helpers used by RMI.
 */
#include "src/rmi/shared/ids.hpp"

#include <cstdint>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>

namespace bdg::bison::rmi::shared {

/**
 * @brief Generate a 32-hex-character identifier from two random 64-bit words.
 *
 * Access to the pseudo-random engine is guarded by a mutex, making this
 * function safe to call concurrently from multiple threads.
 */
std::string generate_id() {
  static std::mutex mtx;
  static std::mt19937_64 gen{std::random_device{}()};
  static std::uniform_int_distribution<uint64_t> dis;

  uint64_t hi, lo;
  {
    std::lock_guard<std::mutex> lk(mtx);
    hi = dis(gen);
    lo = dis(gen);
  }

  std::ostringstream oss;
  oss << std::hex << std::setfill('0') << std::setw(16) << hi
      << std::setw(16) << lo;
  return oss.str();
}

} // namespace bdg::bison::rmi::shared
