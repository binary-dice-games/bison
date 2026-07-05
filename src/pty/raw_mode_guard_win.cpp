// MIT License © 2025 Binary Dice Games
/**
 * @file raw_mode_guard_win.cpp
 * @brief Native Windows implementation of raw_mode_guard using the console
 *        mode API (`GetConsoleMode`/`SetConsoleMode`), the ConPTY-era
 *        equivalent of termios raw mode.
 */
#include "src/pty/raw_mode_guard.hpp"

#include <io.h>
#include <windows.h>

namespace bdg::bison::pty {

/** @brief Saved console mode, only populated when @c fd was a console. */
struct raw_mode_state {
  HANDLE handle{INVALID_HANDLE_VALUE};
  DWORD saved_mode{};
  bool saved{false};
};

raw_mode_guard::raw_mode_guard(int fd) : fd_(fd), state_(std::make_unique<raw_mode_state>()) {
  const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd_));
  if (handle == INVALID_HANDLE_VALUE)
    return;
  DWORD mode{};
  if (!GetConsoleMode(handle, &mode))
    return;
  state_->handle = handle;
  state_->saved_mode = mode;
  state_->saved = true;
  const DWORD raw_mode = mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT |
                                                     ENABLE_PROCESSED_INPUT);
  SetConsoleMode(handle, raw_mode);
}

raw_mode_guard::~raw_mode_guard() {
  if (state_->saved)
    SetConsoleMode(state_->handle, state_->saved_mode);
}

} // namespace bdg::bison::pty
