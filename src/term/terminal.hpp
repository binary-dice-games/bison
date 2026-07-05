// MIT License © 2025 Binary Dice Games
/**
 * @file terminal.hpp
 * @brief RAII wrapper around a spawned interactive pseudo-terminal session,
 *        portable across Linux/MSYS2 (`forkpty()`) and native Windows
 *        (ConPTY).
 *
 * `terminal` is the Windows-capable sibling of `src/pty/pty_process.hpp`:
 * it spawns a child process (a shell by default) attached to a real,
 * interactive pseudo-terminal, and pumps the operator's own real terminal
 * input into it. Unlike `pty_process`, it is not Linux/MSYS2-only — see
 * `terminal_posix.cpp` (forkpty) and `terminal_win.cpp` (ConPTY via
 * `CreatePseudoConsole()`) for the two platform-specific spawn paths.
 *
 * @note Both platform implementations expose the child's pty I/O as plain
 *       CRT file descriptors (`read_handle()`/`write_handle()`), not as a
 *       `uv_stream_t*` bound to a loop this class owns. `uv_pipe_open()`
 *       accepts a CRT fd on both platforms (on Windows, one obtained from a
 *       HANDLE via `_open_osfhandle()`), so `term_transport` — which, like
 *       `stdio_transport`, owns the libuv loop(s) the pty I/O actually runs
 *       on — can wrap either platform's descriptor identically. This keeps
 *       the platform divergence entirely inside `src/term`, and
 *       `term_transport.cpp` free of `#ifdef`.
 * @note Only the inbound direction (operator's real input -> pty) is pumped
 *       by this class. The pty's *output* direction is read by whatever
 *       transport is layered on top of `read_handle()` (see
 *       `src/rmi/transport/term_transport.hpp`) — that transport is the
 *       sole reader. Mirrors the single-reader invariant documented in
 *       `src/pty/DESIGN.md`.
 */
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace bdg::bison::term {

/** @brief Opaque platform-specific state (saved terminal modes, ConPTY handles, ...). */
struct terminal_state;

/**
 * @brief Owns a spawned pty session (forkpty on Linux/MSYS2, ConPTY on
 *        Windows) and an inbound input pump.
 */
class terminal {
 public:
  /**
   * @brief Spawn a child attached to a new pty.
   * @param cmd  Command to exec in the child. Empty string spawns the
   *             operator's `$SHELL` (Linux/MSYS2, falling back to
   *             `/bin/sh`) or `cmd.exe` (Windows).
   * @throws std::runtime_error on spawn/pty allocation failure.
   */
  explicit terminal(const std::string& cmd = {});

  /** @brief Stops the pump, restores the real terminal, closes handles, reaps the child. */
  ~terminal();

  terminal(const terminal&) = delete;
  terminal& operator=(const terminal&) = delete;
  terminal(terminal&&) = delete;
  terminal& operator=(terminal&&) = delete;

  /** @brief Fd to read the child's pty output from. */
  int read_handle() const;

  /** @brief Fd to write the child's pty input to. May equal `read_handle()` (Linux/MSYS2 pty master). */
  int write_handle() const;

  /** @brief Start the background thread that pumps the operator's real input into the pty. */
  void start_pump();

  /** @brief Block until the child exits. @return The child's exit status. */
  int wait();

 private:
  void pump_loop();

  int read_handle_{-1};
  int write_handle_{-1};
  std::atomic<bool> pump_running_{false};
  std::thread pump_thread_;
  std::unique_ptr<terminal_state> state_;
};

} // namespace bdg::bison::term
