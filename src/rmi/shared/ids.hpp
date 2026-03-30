// MIT License © 2025 Binary Dice Games
#pragma once

#include <cstdint>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <string>

namespace bdg::bison::rmi::shared {

/**
 * @brief Generate a cryptographically random 128-bit opaque identifier
 *        encoded as a 32-character lowercase hex string.
 *
 * IDs are non-sequential to prevent guessing attacks across the protocol
 * boundary.  Thread-safe.
 */
inline std::string generate_id() {
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
