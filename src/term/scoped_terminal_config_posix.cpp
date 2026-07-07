// MIT License © 2025 Binary Dice Games
/**
 * @file scoped_terminal_config_posix.cpp
 * @brief Linux/MSYS2 raw-mode half of scoped_terminal_config, using termios.
 */
#include "src/term/scoped_terminal_config.hpp"

#include <termios.h>
#include <unistd.h>

namespace bdg::bison::term {

/** @brief Saved termios state, only populated when read_fd was a terminal. */
struct scoped_terminal_config::impl {
  struct termios saved_termios{};
  bool saved{false};
};

scoped_terminal_config::impl* scoped_terminal_config::create_state(const params& p) {
  auto* state = new impl();

  if (isatty(p.read_fd) == 0)
    return state;
  if (tcgetattr(p.read_fd, &state->saved_termios) != 0)
    return state;

  state->saved = true;
  struct termios raw = state->saved_termios;
  cfmakeraw(&raw);
  tcsetattr(p.read_fd, TCSANOW, &raw);
  return state;
}

void scoped_terminal_config::release_state(impl* state) {
  if (state->saved)
    tcsetattr(params_.read_fd, TCSANOW, &state->saved_termios);
  delete state;
}

} // namespace bdg::bison::term
