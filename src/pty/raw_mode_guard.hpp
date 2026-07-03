// MIT License © 2025 Binary Dice Games
/**
 * @file raw_mode_guard.hpp
 * @brief RAII helper that puts an already-open terminal fd into raw mode for
 *        the lifetime of the guard, restoring the original mode on
 *        destruction.
 *
 * Used by `client_app`'s `--pty` path: when a bison client is launched
 * *inside* a `bison_server --pty` session, its own fd 0/1 are the pty slave
 * that the server-spawned shell was also using. That slave's termios is left
 * in the default cooked mode (see `src/pty/DESIGN.md`) so the shell behaves
 * normally, but cooked-mode processing (`ICANON` line-buffering, `ECHO`)
 * breaks the `BISON<...>` framing that rides over the same fds (see
 * `src/rmi/transport/stdio_transport.hpp`). Putting the slave in raw mode
 * for the duration of the RMI session — and restoring cooked mode
 * afterwards, so the shell keeps working once the client exits — fixes this
 * without touching the server or the shell at all.
 */
#pragma once

#include <memory>

namespace bdg::bison::pty {

/** @brief Opaque platform-specific saved-state. Defined per-platform in the .cpp. */
struct raw_mode_state;

/**
 * @brief Saves @p fd's current terminal mode and switches it to raw mode.
 *
 * Linux: implemented with `tcgetattr`/`cfmakeraw`/`tcsetattr`. If @p fd is
 * not a terminal, construction is a no-op (nothing is saved or changed) and
 * the destructor does nothing.
 *
 * Windows: not implemented — construction is always a no-op, matching
 * `pty_process`'s Linux-only scope.
 */
class raw_mode_guard {
 public:
  /** @param fd Terminal fd to switch to raw mode. */
  explicit raw_mode_guard(int fd);

  /** @brief Restores the fd's original terminal mode, if it was changed. */
  ~raw_mode_guard();

  raw_mode_guard(const raw_mode_guard&) = delete;
  raw_mode_guard& operator=(const raw_mode_guard&) = delete;
  raw_mode_guard(raw_mode_guard&&) = delete;
  raw_mode_guard& operator=(raw_mode_guard&&) = delete;

 private:
  int fd_;
  std::unique_ptr<raw_mode_state> state_;
};

} // namespace bdg::bison::pty
