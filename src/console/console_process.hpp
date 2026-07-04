// MIT License © 2025 Binary Dice Games
/**
 * @file console_process.hpp
 * @brief RAII wrapper around a libuv-spawned subprocess with piped stdio.
 *
 * `console_process` spawns a command line via `uv_spawn()` (through
 * `/bin/sh -c`) with its stdin/stdout connected to a pair of pipes, and
 * exposes the parent-side ends of those pipes as plain fds. Unlike
 * `pty_process` (`src/pty/pty_process.hpp`), no pty is involved and no
 * operator terminal is touched or pumped — this is purely a subprocess
 * lifecycle primitive for the non-interactive `--transport=console` mode.
 * The framing that rides on top of `read_fd()`/`write_fd()` lives in
 * `src/rmi/transport/stdio_transport.hpp`, exactly as it does for
 * `pty_process`'s `master_fd()`.
 */
#pragma once

#include "src/bison/bison.hpp"

#include <memory>
#include <optional>
#include <string>

namespace bdg::bison::console {

/** @brief Opaque libuv-backed process/pipe state. Defined in the .cpp. */
struct console_process_state;

/**
 * @brief Owns a subprocess spawned via `uv_spawn()` with piped stdio.
 *
 * The subprocess is run as `/bin/sh -c "<cmd>"`, so `cmd` may be an
 * arbitrary shell command line (quoting, pipes, `ssh ... other-command`,
 * etc. all work exactly as they would when typed at a shell prompt).
 */
class console_process {
 public:
  /**
   * @brief Spawn `cmd` via `/bin/sh -c`, with its stdin/stdout piped.
   * @param cmd  Shell command line to run. Must not be empty.
   * @throws std::runtime_error if `uv_spawn()` fails (e.g. `/bin/sh` is
   *         missing).
   */
  explicit console_process(const std::string& cmd);

  /** @brief Requests the child stop (if still running), then joins the I/O thread. */
  ~console_process();

  console_process(const console_process&) = delete;
  console_process& operator=(const console_process&) = delete;
  console_process(console_process&&) = delete;
  console_process& operator=(console_process&&) = delete;

  /** @brief Fd to read the child's stdout from. */
  int read_fd() const;

  /** @brief Fd to write the child's stdin to. */
  int write_fd() const;

  /**
   * @brief Block until the child exits.
   * @return The child's exit code, or `128 + signal` if it was killed by a
   *         signal. Safe to call more than once; later calls return the
   *         same value.
   */
  int wait();

 private:
  std::unique_ptr<console_process_state> state_;
  int read_fd_{-1};
  int write_fd_{-1};
};

} // namespace bdg::bison::console
