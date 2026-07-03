// MIT License © 2025 Binary Dice Games
/**
 * @file pty_process.hpp
 * @brief RAII wrapper around a forked pseudo-terminal session.
 *
 * `pty_process` spawns a child process (a shell by default) attached to a
 * freshly allocated pty, and puts the caller's own real terminal (fd 0) into
 * raw mode so that `start_pump()` can forward the operator's keystrokes into
 * the pty byte-for-byte, with no line buffering or local echo duplication.
 *
 * @note Only the inbound direction (operator stdin → pty master) is pumped
 *       by this class. The pty master's *output* direction is read by
 *       whatever transport is layered on top of `master_fd()` (see
 *       `src/rmi/transport/stdio_transport.hpp`) — that transport is the
 *       sole reader of the master fd. See `src/pty/DESIGN.md` for the
 *       full data-flow rationale.
 */
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace bdg::bison::pty {

/** @brief Platform-specific saved terminal state. Defined per-platform in the .cpp. */
struct pty_process_state;

/**
 * @brief Owns a forked pty session and an inbound keystroke pump.
 *
 * Linux: implemented with `forkpty()`. The operator's real terminal (fd 0)
 * is switched to raw mode for the lifetime of this object and restored on
 * destruction.
 *
 * Windows: not implemented — the constructor always throws
 * `std::runtime_error`.
 *
 * Windows' ConPTY is not a transparent byte pipe like a Unix pty — it's a full terminal emulator that maintains its own
 * virtual screen buffer and actively rewrites the byte stream to keep that buffer in sync (injecting cursor-position
 * and redraw escape sequences), even for a "raw mode" session. This means arbitrary in-band binary/base64 payloads sent
 * over the --pty-tunneled stdio transport can be corrupted in place whenever a line approaches the console width, and —
 * as confirmed by trace analysis — that corruption is often deterministic for a given cursor column, so even a
 * NACK-and-resend retry can fail identically on every attempt. Mitigations (line-wrapping the payload, frame
 * sequencing/NACK-resend, waiting for disconnect acknowledgment) reduced the failure rate but could not eliminate it,
 * because there is no way from the application side to make ConPTY stop rewriting the stream. Real Unix ptys (Linux,
 * and by extension the pty_process_linux.cpp path) don't have this problem: they are a genuine transparent character
 * pipe with no independent screen-buffer emulation between the two ends. Given that, restricting --pty support to Linux
 * is the right call — it avoids chasing an unbounded set of ConPTY corruption cases on a platform that isn't built for
 * this use case.
 */
class pty_process {
 public:
  /**
   * @brief Fork a child attached to a new pty.
   * @param cmd  Command to exec in the child. Empty string spawns the
   *             operator's `$SHELL` (falling back to `/bin/sh`).
   * @throws std::runtime_error on fork/pty allocation failure, or
   *         unconditionally on Windows ("not implemented").
   */
  explicit pty_process(const std::string& cmd = {});

  /** @brief Stops the pump, restores the real terminal, closes fds, reaps the child. */
  ~pty_process();

  pty_process(const pty_process&) = delete;
  pty_process& operator=(const pty_process&) = delete;
  pty_process(pty_process&&) = delete;
  pty_process& operator=(pty_process&&) = delete;

  /** @brief The pty master fd. Output from the child is read from this fd. */
  int master_fd() const;

  /** @brief Start the background thread that pumps operator stdin into the pty master. */
  void start_pump();

  /** @brief Block until the child exits. @return The child's exit status. */
  int wait();

 private:
  void pump_loop();

  int master_fd_{-1};
  int child_pid_{-1};
  std::atomic<bool> pump_running_{false};
  std::thread pump_thread_;
  std::unique_ptr<pty_process_state> state_;
};

} // namespace bdg::bison::pty
