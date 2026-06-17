// MIT License © 2025 Binary Dice Games
/**
 * @file pty_server_transport.hpp
 * @brief PTY-owning server transport for multi-session RMI over terminal channels.
 */
#pragma once

#if defined(__linux__)

#include "src/bison/bison.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace bdg::bison::app {

/**
 * @brief Per-session server connection backed by the PTY master fd.
 *
 * Shares the PTY master fd and inbox queue with `pty_server_transport`.
 * Exactly one instance exists per active session; destroyed when the
 * session ends so all session objects are cleaned up.
 */
class pty_server_connection : public rmi::transport::server_connection_iface {
 public:
  struct impl;

  explicit pty_server_connection(std::unique_ptr<impl> impl);
  ~pty_server_connection();

  pty_server_connection(pty_server_connection&&) noexcept;
  pty_server_connection& operator=(pty_server_connection&&) noexcept;

  pty_server_connection(const pty_server_connection&) = delete;
  pty_server_connection& operator=(const pty_server_connection&) = delete;

  /**
   * @brief Encode @p frame as a DCS data message and write it to the PTY master.
   * @throws std::runtime_error if the session is closed or the write fails.
   */
  void send(bison::buffer frame) override;

  /**
   * @brief Block until a frame arrives from the client or the session closes.
   * @param timeout Maximum wait duration.
   * @return `true` when a frame was received; `false` on timeout or close.
   */
  bool receive(
      bison::buffer& frame,
      std::chrono::milliseconds timeout) override;

  /** @brief Mark the session as closed and wake any blocked receivers. */
  void close() override;

  /** @brief Return `true` if the session has been closed. */
  bool is_closed() const override;

 private:
  std::unique_ptr<impl> impl_;
};

/**
 * @brief Server-side transport that owns a PTY subprocess and relays terminal I/O.
 *
 * Forks a shell via `forkpty`, switches the caller's terminal to raw/no-echo
 * mode, and multiplexes two streams over the same PTY master fd:
 *
 * - **Terminal relay** — plaintext bytes from the PTY slave are forwarded to
 *   `stdout` so the user sees normal shell output; user `stdin` keystrokes are
 *   relayed to the PTY slave under `write_mtx_`.
 * - **Bison channel** — DCS frames from the PTY slave are extracted by the
 *   reader thread and queued as bison buffers for `accept()` / `receive()`.
 *
 * Supports multiple sequential client sessions: after one session ends call
 * `restart_session()` and the transport waits for the next client HELLO without
 * restarting the shell subprocess.
 *
 * Linux only.
 */
class pty_server_transport : public rmi::transport::server_transport_iface {
 public:
  /**
   * @brief Construct the transport.
   * @param shell Shell command to launch via `forkpty` (default: "bash").
   */
  explicit pty_server_transport(std::string shell = "bash");
  ~pty_server_transport();

  pty_server_transport(const pty_server_transport&) = delete;
  pty_server_transport& operator=(const pty_server_transport&) = delete;

  /**
   * @brief Fork the shell and start the reader and input-relay threads.
   *
   * Idempotent — subsequent calls while the shell is running are no-ops.
   * Does NOT emit a HELLO frame; the client initiates the handshake.
   *
   * @param params Optional transport configuration; mode=dcs is forced.
   * @throws std::runtime_error if `forkpty` fails.
   */
  void start(bison::dynamic params) override;

  /**
   * @brief Wait for the client's HELLO, respond, and return the connection.
   *
   * Blocks until `pty_client_transport::open()` on the remote side sends a
   * HELLO frame through the PTY channel, or until @p timeout elapses or the
   * shell exits.  On success, responds with the server's own HELLO so the
   * client's `open()` call unblocks, then returns the connection.
   *
   * After returning a non-null connection, subsequent calls return `nullptr`
   * until `restart_session()` resets the state.
   *
   * @param timeout Maximum wait duration.
   * @return Live connection on success; `nullptr` on timeout or shell exit.
   */
  std::unique_ptr<rmi::transport::server_connection_iface> accept(
      std::chrono::milliseconds timeout) override;

  /**
   * @brief Shut down the transport: signal the shell, close the PTY, restore tty.
   *
   * Detaches the reader and input-relay threads (they may be blocked in
   * syscalls).  Single-use — do not call `start()` again after `stop()`.
   */
  void stop() override;

  /**
   * @brief Prepare for the next client session without restarting the shell.
   *
   * Clears the inbox and resets session atomics so `accept()` will wait for
   * the next client HELLO.  Must only be called after the previous session's
   * `rmi::server` has been destroyed.
   */
  void restart_session();

  /**
   * @brief Return `true` while the shell subprocess is still running.
   */
  bool is_shell_running() const;

  /**
   * @brief Block until the active session connection is closed or timeout elapses.
   * @param timeout Maximum wait duration.
   * @return `true` when closed; `false` on timeout.
   */
  bool wait_until_closed(std::chrono::milliseconds timeout) const;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace bdg::bison::app

#endif // defined(__linux__)
