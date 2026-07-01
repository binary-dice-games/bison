// MIT License © 2025 Binary Dice Games
/**
 * @file pty_client_transport.hpp
 * @brief RMI client transport that communicates via DCS framing on the
 *        process's own stdin/stdout (the PTY slave fds).
 *
 * This transport is used by an RMI client program that was launched inside a
 * PTY by `pty_server_transport`.  Its stdin and stdout are already the PTY
 * slave — `pty_client_transport` wraps them without allocating any PTY itself.
 *
 * DCS frames arriving on stdin are decoded and placed in the inbox queue.
 * Non-DCS bytes on stdin are routed to the optional `on_plain` callback (or
 * discarded silently).  Outbound RMI frames are encoded as DCS and written to
 * stdout.
 */
#pragma once

#if defined(__linux__) || defined(_WIN32)

#include "src/rmi/transport/transport_iface.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

namespace bdg {
namespace bison {
namespace rmi {
namespace transport {

/** @brief Opaque per-transport state; defined in platform .cpp. */
struct pty_client_impl;

/**
 * @brief Client transport that wraps stdin/stdout for DCS framing.
 *
 * @note Call `open()` before `send()` or `receive()`.  The transport reads
 *       from `STDIN_FILENO` / `STD_INPUT_HANDLE` and writes to
 *       `STDOUT_FILENO` / `STD_OUTPUT_HANDLE`.
 */
class pty_client_transport : public client_transport_iface {
 public:
  using plain_cb = std::function<void(uint8_t)>;

  /**
   * @param on_plain Callback for non-DCS bytes from stdin.  Leave empty to
   *                 discard them silently.
   */
  explicit pty_client_transport(plain_cb on_plain = {});
  ~pty_client_transport();

  pty_client_transport(const pty_client_transport&) = delete;
  pty_client_transport& operator=(const pty_client_transport&) = delete;

  /**
   * @brief Start the background read thread and send a HELLO handshake.
   * @param params  Ignored.
   */
  void open(bison::dynamic params) override;

  /**
   * @brief Encode @p frame as DCS and write it to stdout.
   * @throws std::runtime_error on write failure or if not yet opened.
   */
  void send(bison::buffer frame) override;

  /**
   * @brief Block until a DCS frame from stdin is available.
   * @param frame   Populated on success.
   * @param timeout Maximum wait before returning `false`.
   * @return `true` on success; `false` on timeout, EOF, or shutdown.
   */
  bool receive(bison::buffer& frame,
               std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Stop the read thread; subsequent sends/receives are no-ops. */
  void shutdown() override;

  /** @brief Return `false` after `shutdown()` or EOF on stdin. */
  bool is_connected() const override;

 private:
  std::unique_ptr<pty_client_impl> impl_;
};

} // namespace transport
} // namespace rmi
} // namespace bison
} // namespace bdg

#endif // defined(__linux__) || defined(_WIN32)
