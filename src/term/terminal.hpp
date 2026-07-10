// MIT License © 2025 Binary Dice Games
/**
 * @file terminal.hpp
 * @brief RAII wrapper around a spawned interactive pseudo-terminal session,
 *        portable across Linux/MSYS2 (`forkpty()`) and native Windows
 *        (ConPTY).
 *
 * `terminal` spawns a child process (a shell by default) attached to a real,
 * interactive pseudo-terminal, and pumps the operator's own real terminal
 * input into it.
 *
 * @note For the object's lifetime, fd 1 (stdout) and fd 2 (stderr) are also
 *       transparently redirected through a byte-level `'\n'` -> `"\r\n"`
 *       translator before reaching the real terminal. Raw mode (set by the
 *       constructor) disables the tty's own newline translation, so without
 *       this any writer that doesn't know to ask for special handling --
 *       C stdio (`printf`/`fprintf`), direct `write()`, third-party
 *       libraries -- would otherwise "staircase" on screen. Translation
 *       happens as bytes are received, not buffered until a full line, so
 *       output (including partial, non-newline-terminated writes) appears
 *       immediately.
 *
 * @note Both platform implementations expose the child's pty I/O as plain
 *       CRT file descriptors (`read_handle()`/`write_handle()`), not as a
 *       `uv_stream_t*` bound to a loop this class owns. `uv_pipe_open()`
 *       accepts a CRT fd on both platforms (on Windows, one obtained from a
 *       HANDLE via `_open_osfhandle()`), so `term_transport` — which owns the
 *       libuv loop(s) the pty I/O actually runs on — can wrap either platform's
 *       descriptor identically. This keeps the platform divergence entirely
 *       inside `src/term`, and `term_transport.cpp` free of `#ifdef`.
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
   * @param prompt_label  Optional short label (e.g. "wish-server") used to
   *             override the child shell's prompt, so a spawned terminal is
   *             visually distinguishable from the operator's own. Empty
   *             string (default) leaves the shell's own prompt untouched.
   *             Best-effort: reliable on bash (via `PROMPT_COMMAND`) and
   *             `cmd.exe` (via `PROMPT`); shells without either mechanism
   *             (e.g. zsh, fish) are unaffected.
   * @throws std::runtime_error on spawn/pty allocation failure.
   */
  explicit terminal(const std::string& cmd = {}, const std::string& prompt_label = {});

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

  /**
   * @brief Non-blocking check for whether the child has exited.
   *
   * Lets a caller poll for exit alongside other shutdown conditions (e.g. a
   * renderer's own close signal) instead of blocking in `wait()`. Reaps the
   * child as a side effect once it has exited, so a later `wait()` or the
   * destructor's own reap will not double-`waitpid()`/re-wait on it.
   *
   * @return `true` once the child has exited (or was already reaped);
   *         `false` if it is still running.
   */
  bool has_exited();

 private:
  void pump_loop();
  void stdio_pump_loop();

  int read_handle_{-1};
  int write_handle_{-1};
  std::atomic<bool> pump_running_{false};
  std::thread pump_thread_;
  std::unique_ptr<terminal_state> state_;
};

} // namespace bdg::bison::term
