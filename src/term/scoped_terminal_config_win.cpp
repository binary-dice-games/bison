// MIT License © 2025 Binary Dice Games
/**
 * @file scoped_terminal_config_win.cpp
 * @brief Native Windows raw-mode half of scoped_terminal_config, using the
 *        console mode API (`GetConsoleMode`/`SetConsoleMode`).
 */
#include "src/term/scoped_terminal_config.hpp"

#include <io.h>
#include <windows.h>

namespace bdg::bison::term {

/** @brief Saved console mode, only populated when read_fd was a console. */
struct scoped_terminal_config::impl {
  HANDLE handle{INVALID_HANDLE_VALUE};
  DWORD saved_mode{};
  bool saved{false};
};

scoped_terminal_config::impl* scoped_terminal_config::create_state(const params& p) {
  auto* state = new impl();

  const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(p.read_fd));
  if (handle == INVALID_HANDLE_VALUE)
    return state;
  DWORD mode{};
  if (!GetConsoleMode(handle, &mode))
    return state;

  state->handle = handle;
  state->saved_mode = mode;
  state->saved = true;
  const DWORD raw_mode = mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT |
                                                     ENABLE_PROCESSED_INPUT);
  SetConsoleMode(handle, raw_mode);
  return state;
}

void scoped_terminal_config::release_state(impl* state) {
  if (state->saved)
    SetConsoleMode(state->handle, state->saved_mode);
  delete state;
}

} // namespace bdg::bison::term
