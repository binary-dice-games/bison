// MIT License © 2025 Binary Dice Games
/**
 * @file pty_process.hpp
 * @brief OS-level pseudoterminal process factory.
 *
 * Spawns a child process inside a real PTY (Linux: forkpty; Windows: ConPTY)
 * so that `isatty(stdin) == true` in the child.  This is the sole purpose of
 * this module — it knows nothing about libuv, DCS framing, or the RMI
 * protocol.
 *
 * Platform note: the accessor methods (`master_fd`, `h_out_read`, `h_in_write`)
 * are declared conditionally because the underlying handle types differ between
 * POSIX and Win32.  This is a type-declaration necessity, not behavioral
 * branching — behavior is split across pty_process_linux.cpp and
 * pty_process_win.cpp.
 */
#pragma once

#if defined(__linux__) || defined(_WIN32)

#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace bdg::bison::rmi::pty {

/** @brief Configuration for spawning a child process inside a PTY. */
struct pty_config {
  std::string cmd;               ///< Executable path or name.
  std::vector<std::string> args; ///< Arguments (not including argv[0]).
  uint16_t cols{80};             ///< Initial PTY column count.
  uint16_t rows{24};             ///< Initial PTY row count.
};

/**
 * @brief Move-only handle for a child process running inside a real PTY.
 *
 * The constructor spawns the child immediately.  Destruction does NOT wait
 * for or kill the child — call `terminate()` and then `wait()` explicitly
 * when shutdown is needed.
 *
 * @par isatty invariant
 * In the spawned child `isatty(0) == true` and `isatty(1) == true`.
 * Shells, REPLs, readline, and any `isatty()`-checking code will behave as
 * if attached to a real terminal.
 */
class pty_process {
 public:
  /**
   * @brief Spawn @p cfg.cmd inside a new PTY.
   * @throws std::runtime_error if the PTY allocation or exec fails.
   */
  explicit pty_process(pty_config cfg);
  ~pty_process();

  pty_process(pty_process&&) noexcept;
  pty_process& operator=(pty_process&&) noexcept;

  pty_process(const pty_process&) = delete;
  pty_process& operator=(const pty_process&) = delete;

#if defined(_WIN32)
  /** @brief Handle for reading child stdout (ConPTY output pipe). */
  HANDLE h_out_read() const noexcept;
  /** @brief Handle for writing to child stdin (ConPTY input pipe). */
  HANDLE h_in_write() const noexcept;
  /**
   * @brief Transfer ownership of both ConPTY handles to the caller.
   * After this call h_out_read() and h_in_write() return INVALID_HANDLE_VALUE
   * and the destructor will not close them.
   */
  void release_handles(HANDLE& out_read, HANDLE& in_write) noexcept;
#else
  /** @brief Bidirectional PTY master file descriptor. */
  int master_fd() const noexcept;
  /**
   * @brief Transfer ownership of the master fd to the caller.
   * After this call master_fd() returns -1 and the destructor will not
   * close it.  The caller is responsible for closing the returned fd.
   */
  int release_master_fd() noexcept;
#endif

  /**
   * @brief Block until the child exits and return its exit code.
   * @return Exit code (0 on normal exit; -1 if the status is unavailable).
   */
  int wait();

  /**
   * @brief Send a termination signal (SIGTERM / TerminateProcess) to the child.
   * Does nothing if the child has already exited.
   */
  void terminate();

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace bdg::bison::rmi::pty

#endif // defined(__linux__) || defined(_WIN32)
