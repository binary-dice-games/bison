// MIT License © 2025 Binary Dice Games
/**
 * @file stdio_transport.hpp
 * @brief RMI transport that frames envelopes as base64 lines over a pair of
 *        raw fds (typically stdin/stdout, or a pty master fd).
 *
 * Wire framing: `\nBISON:<base64(frame)>\n`. Bytes that are not part of a
 * confirmed `BISON:` line are forwarded verbatim, as soon as they arrive, to
 * a caller-supplied passthrough callback — this is what keeps a pty session
 * fully interactive (see `src/bison/pty/DESIGN.md`) instead of behaving like
 * a line-buffered reader. See `FORMAT.md` for the full framing contract.
 *
 * Implemented with libuv (matching `pipe_transport`/`named_pipe_transport`/
 * `socket_transport`), so this file has no platform-specific translation
 * units — wrapping an already-open fd is identical on Linux and Windows.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string_view>

namespace bdg::bison::rmi::transport {

/** @brief Opaque libuv-backed connection state. Defined in the .cpp. */
struct stdio_conn_state;

/** @brief Callback invoked with each chunk of non-`BISON:` bytes, in arrival order. */
using stdio_passthrough_cb = std::function<void(std::string_view chunk)>;

/** @brief Default passthrough: write the chunk verbatim to stdout. */
void stdio_print_passthrough(std::string_view chunk);

/** @brief Default passthrough: discard the chunk. */
void stdio_discard_passthrough(std::string_view chunk);

// ── Client-side transport ─────────────────────────────────────────────────────

/**
 * @brief Client transport that wraps a pair of already-open fds.
 *
 * Typically constructed directly from the process's own inherited `fd 0`
 * (read) / `fd 1` (write) — see `client_app`'s `--pty` handling.
 */
class stdio_client_transport : public client_transport_iface {
 public:
  /**
   * @param read_fd      Fd to read peer bytes from.
   * @param write_fd     Fd to write frames and pass-through bytes to.
   * @param passthrough  Called with non-`BISON:` bytes read from `read_fd`.
   */
  stdio_client_transport(int read_fd, int write_fd, stdio_passthrough_cb passthrough = stdio_discard_passthrough);
  ~stdio_client_transport() override;

  /** @brief Starts the background I/O loops. The fds must already be open. */
  void open(bison::dynamic params) override;

  /** @brief Base64-wrap and write one frame as a `BISON:` line. */
  void send(bison::buffer frame) override;

  /**
   * @brief Wait for the next decoded `BISON:` frame.
   * @param frame   Output frame buffer.
   * @param timeout Maximum wait before returning `false`.
   * @return `true` on success; `false` on timeout, EOF, or shutdown.
   */
  bool receive(bison::buffer& frame, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Stop the background I/O loops; pending and future receives return `false`. */
  void shutdown() override;

 private:
  std::unique_ptr<stdio_conn_state> state_;
};

// ── Server-side connection ────────────────────────────────────────────────────

/**
 * @brief Server-side connection backed by a pair of already-open fds.
 *
 * Produced by `stdio_server_transport::accept()`.
 */
class stdio_server_connection : public server_connection_iface {
 public:
  explicit stdio_server_connection(std::unique_ptr<stdio_conn_state> state);
  ~stdio_server_connection() override;

  /** @brief Base64-wrap and write one frame as a `BISON:` line. */
  void send(bison::buffer frame) override;

  /**
   * @brief Wait for the next decoded `BISON:` frame.
   * @param frame   Output frame buffer.
   * @param timeout Maximum wait before returning `false`.
   * @return `true` on success; `false` on timeout, EOF, or close.
   */
  bool receive(bison::buffer& frame, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Stop the background I/O loops; pending and future receives return `false`. */
  void close() override;

  /** @brief Return whether `close()` has been called. */
  bool is_closed() const override;

 private:
  std::unique_ptr<stdio_conn_state> state_;
};

// ── Server-side transport (single-connection listener) ────────────────────────

/**
 * @brief Server transport that vends a single connection over a pair of
 *        already-open fds (e.g. a pty master fd for both directions).
 *
 * Mirrors `stream_server_transport`: `accept()` returns the one connection
 * on the first call and `nullptr` on every subsequent call.
 */
class stdio_server_transport : public server_transport_iface {
 public:
  /**
   * @param read_fd      Fd to read peer bytes from.
   * @param write_fd     Fd to write frames and pass-through bytes to. May
   *                     equal `read_fd` (e.g. a pty master fd).
   * @param passthrough  Called with non-`BISON:` bytes read from `read_fd`.
   */
  stdio_server_transport(int read_fd, int write_fd, stdio_passthrough_cb passthrough = stdio_print_passthrough);

  stdio_server_transport(const stdio_server_transport&) = delete;
  stdio_server_transport& operator=(const stdio_server_transport&) = delete;

  /** @brief No-op; the fds must already be open. */
  void start(bison::dynamic params) override;

  /**
   * @brief Return the one connection on the first call; `nullptr` thereafter.
   * @param timeout  Ignored; connection setup is synchronous.
   */
  std::unique_ptr<server_connection_iface> accept(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Mark the transport stopped; future accept calls return nullptr. */
  void stop() override;

 private:
  int read_fd_;
  int write_fd_;
  stdio_passthrough_cb passthrough_;
  std::atomic<bool> stopped_{false};
  std::atomic<bool> accepted_{false};
};

} // namespace bdg::bison::rmi::transport
