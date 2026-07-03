// MIT License © 2025 Binary Dice Games
/**
 * @file raw_mode_guard_win.cpp
 * @brief Windows stub for raw_mode_guard — not implemented.
 */
#include "src/pty/raw_mode_guard.hpp"

namespace bdg::bison::pty {

/** @brief Unused on Windows; construction is always a no-op. */
struct raw_mode_state {};

raw_mode_guard::raw_mode_guard(int fd) : fd_(fd), state_(std::make_unique<raw_mode_state>()) {}

raw_mode_guard::~raw_mode_guard() = default;

} // namespace bdg::bison::pty
