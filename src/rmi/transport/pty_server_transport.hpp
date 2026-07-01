// MIT License © 2025 Binary Dice Games
/**
 * @file pty_server_transport.hpp
 * @brief RMI server transport that drives a PTY child process over DCS framing.
 *
 * `pty_server_transport::start()` spawns the configured child process inside a
 * real PTY.  `accept()` returns the single `pty_server_connection` wrapping
 * that process.  DCS escape sequences multiplex RMI frames over the PTY byte
 * stream; plain (non-DCS) bytes from the child are routed to an `on_plain`
 * callback supplied at construction time.
 *
 * @note The PTY transport models exactly one connection per transport instance
 *       (one spawned process).  After the connection is accepted once, further
 *       `accept()` calls return `nullptr`.
 */
#pragma once

#if defined(__linux__) || defined(_WIN32)

#include "src/rmi/pty/pty_process.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

namespace bdg {
namespace bison {
namespace rmi {
namespace transport {

/** @brief Opaque per-connection state; defined in platform .cpp. */
struct pty_server_conn_impl;

// ── pty_server_connection ─────────────────────────────────────────────────────

/**
 * @brief Server-side connection backed by a PTY child process.
 *
 * Produced by `pty_server_transport::accept()`.  `send()` writes DCS frames
 * to the PTY master (child's stdin).  `receive()` blocks until the child
 * emits a DCS frame via its stdout.
 */
class pty_server_connection : public server_connection_iface {
 public:
  explicit pty_server_connection(std::unique_ptr<pty_server_conn_impl> impl);
  ~pty_server_connection();

  pty_server_connection(const pty_server_connection&) = delete;
  pty_server_connection& operator=(const pty_server_connection&) = delete;

  /**
   * @brief Encode @p frame as DCS and write it to the PTY master.
   * @throws std::runtime_error on write failure.
   */
  void send(bison::buffer frame) override;

  /**
   * @brief Block until a DCS frame from the child is available.
   * @param frame   Populated on success.
   * @param timeout Maximum wait before returning `false`.
   * @return `true` on success; `false` on timeout, EOF, or close.
   */
  bool receive(bison::buffer& frame,
               std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Stop the read thread and mark the connection closed. */
  void close() override;

  /** @brief Return whether `close()` has been called or the child has exited. */
  bool is_closed() const override;

 private:
  std::unique_ptr<pty_server_conn_impl> impl_;
};

// ── pty_server_transport ──────────────────────────────────────────────────────

/**
 * @brief Server transport that spawns a child process inside a real PTY.
 *
 * @par Usage
 * @code
 * pty_server_transport srv{
 *   {.cmd="/bin/bash", .args={}, .cols=220, .rows=50},
 *   [](uint8_t b) { /* forward plain byte to user terminal *\/ }
 * };
 * srv.start(bison::dynamic{});
 * auto conn = srv.accept(std::chrono::seconds{5});
 * @endcode
 */
class pty_server_transport : public server_transport_iface {
 public:
  using plain_cb = std::function<void(uint8_t)>;
  using closed_cb = std::function<void()>;

  /**
   * @param cfg       Child process configuration (cmd, args, terminal size).
   * @param on_plain  Callback for non-DCS bytes from the child (shell prompts,
   *                  command output).  May be empty to discard plain output.
   * @param on_closed Called from the read thread as soon as the child process
   *                  exits (EOF on the PTY master / ConPTY output pipe).  Use
   *                  this to trigger server shutdown rather than relying on the
   *                  RMI session-destroyed hook, which may fire later or not at
   *                  all if the child exits without a clean RMI disconnect.
   */
  explicit pty_server_transport(pty::pty_config cfg, plain_cb on_plain = {},
                                closed_cb on_closed = {});
  ~pty_server_transport();

  pty_server_transport(const pty_server_transport&) = delete;
  pty_server_transport& operator=(const pty_server_transport&) = delete;

  /**
   * @brief Spawn the child process inside a new PTY.
   * @param params  Ignored; configuration comes from the constructor.
   * @throws std::runtime_error if spawning fails.
   */
  void start(bison::dynamic params) override;

  /**
   * @brief Return the single connection on the first call; `nullptr` thereafter.
   * @param timeout Ignored — the connection is available immediately after `start()`.
   */
  std::unique_ptr<server_connection_iface> accept(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /**
   * @brief Terminate the child process; pending and future `accept()` return nullptr.
   */
  void stop() override;

 private:
  pty::pty_config cfg_;
  plain_cb on_plain_;
  closed_cb on_closed_;
  std::unique_ptr<pty::pty_process> process_;
  std::atomic<bool> accepted_{false};
  std::atomic<bool> stopped_{false};
};

} // namespace transport
} // namespace rmi
} // namespace bison
} // namespace bdg

#endif // defined(__linux__) || defined(_WIN32)
