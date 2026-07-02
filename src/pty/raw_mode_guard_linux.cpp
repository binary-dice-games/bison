// MIT License © 2025 Binary Dice Games
/**
 * @file raw_mode_guard_linux.cpp
 * @brief Linux implementation of raw_mode_guard using termios.
 */
#include "src/pty/raw_mode_guard.hpp"

#include <termios.h>
#include <unistd.h>

namespace bdg::bison::pty {

/** @brief Saved termios state, only populated when @c fd was a terminal. */
struct raw_mode_state {
  struct termios saved_termios {};
  bool saved{false};
};

raw_mode_guard::raw_mode_guard(int fd) : fd_(fd), state_(std::make_unique<raw_mode_state>()) {
  if (isatty(fd_) == 0)
    return;
  if (tcgetattr(fd_, &state_->saved_termios) != 0)
    return;
  state_->saved = true;
  struct termios raw = state_->saved_termios;
  cfmakeraw(&raw);
  tcsetattr(fd_, TCSANOW, &raw);
}

raw_mode_guard::~raw_mode_guard() {
  if (state_->saved)
    tcsetattr(fd_, TCSANOW, &state_->saved_termios);
}

} // namespace bdg::bison::pty
