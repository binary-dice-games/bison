// MIT License © 2025 Binary Dice Games
/**
 * @file stdio_transport.hpp
 * @brief RMI transport that frames envelopes as base64 lines over a pair of
 *        raw fds (typically stdin/stdout, or a pty master fd).
 *
 * Wire framing: `\nBISON:<base64(frame)>\n`. Bytes that are not part of a
 * confirmed `BISON:` line are forwarded verbatim, as soon as they arrive, to
 * a caller-supplied passthrough callback — this is what keeps a pty session
 * fully interactive (see `src/pty/DESIGN.md`) instead of behaving like
 * a line-buffered reader. See `FORMAT.md` for the full framing contract.
 *
 * Implemented with libuv (matching `pipe_transport`/`named_pipe_transport`/
 * `socket_transport`). This header and `stdio_transport.cpp` are themselves
 * platform-independent; the one platform difference (duplicating an fd, so
 * that closing a connection never closes the caller's original fd — see
 * `dup_stdio_fd`'s doc comment in `stdio_transport.cpp`) is split out to
 * `stdio_transport_linux.cpp`/`stdio_transport_win.cpp`, matching
 * `socket_transport`'s `duplicate_tcp_socket` split.
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

/**
 * @brief Callback invoked with each chunk of non-`BISON:` bytes, in arrival
 *        order. Called once more with an empty chunk when the read side
 *        closes (EOF or error), as a stream-closed signal; this never
 *        happens mid-stream otherwise since empty reads are not forwarded.
 */
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
 * Produced by `stdio_server_transport::accept()`. Wraps a *shared* reference
 * to the transport's one persistent `stdio_conn_state` (see
 * `stdio_server_transport`'s doc comment for why it's shared rather than
 * owned outright) — `close()` only ends this logical session and notifies
 * the transport it can vend a new connection; it never tears down the
 * underlying reader/writer or their fds. Only `stdio_server_transport::stop()`
 * does that.
 */
class stdio_server_connection : public server_connection_iface {
 public:
  /**
   * @param state    The transport's shared, persistent I/O state.
   * @param on_close Invoked once, the first time `close()` runs (directly or
   *                 via the destructor), so the transport can mark itself
   *                 available for the next `accept()`.
   */
  stdio_server_connection(std::shared_ptr<stdio_conn_state> state, std::function<void()> on_close);
  ~stdio_server_connection() override;

  /** @brief Base64-wrap and write one frame as a `BISON:` line. */
  void send(bison::buffer frame) override;

  /**
   * @brief Wait for the next decoded `BISON:` frame.
   * @param frame   Output frame buffer.
   * @param timeout Maximum wait before returning `false`.
   * @return `true` on success; `false` on timeout or close (this
   *         connection's or the underlying transport's).
   */
  bool receive(bison::buffer& frame, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief End this logical session; does not stop the shared reader/writer. */
  void close() override;

  /** @brief Return whether `close()` has been called on this connection. */
  bool is_closed() const override;

 private:
  std::shared_ptr<stdio_conn_state> state_;
  std::function<void()> on_close_;
  std::atomic<bool> closed_{false};
};

// ── Server-side transport (persistent reader, sequential connections) ─────────

/**
 * @brief Server transport over a pair of already-open fds (e.g. a pty master
 *        fd for both directions), supporting sequential (not concurrent)
 *        connections.
 *
 * `start()` opens one persistent `stdio_conn_state` — reader and writer
 * loops over `read_fd_`/`write_fd_` — that lives for the transport's whole
 * lifetime, independent of any individual connection. This matters for the
 * pty use case (see `src/pty/DESIGN.md`): the reader is what drains the pty
 * master and keeps forwarding the shell's own output to the passthrough
 * callback, and that has to keep happening whether or not an RMI client is
 * currently connected, or the operator's shell would go silent (and,
 * eventually, block on a full pty buffer) the moment one client session
 * ends.
 *
 * `accept()` hands out at most one *checked-out* connection at a time: while
 * one is outstanding, further calls return `nullptr` (matching
 * `stream_server_transport`'s poll-and-retry contract); once that
 * connection's `close()` runs, the next `accept()` call succeeds again. This
 * lets an operator run the RMI client, exit back to the shell, and run it
 * again — each run is its own connection and its own `client_worker` session
 * server-side — without restarting the pty or the server.
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
  ~stdio_server_transport() override;

  stdio_server_transport(const stdio_server_transport&) = delete;
  stdio_server_transport& operator=(const stdio_server_transport&) = delete;

  /** @brief Opens the persistent reader/writer over `read_fd_`/`write_fd_`. */
  void start(bison::dynamic params) override;

  /**
   * @brief Return a new connection if none is currently checked out;
   *        `nullptr` otherwise (including after `stop()`).
   * @param timeout  Ignored; connection setup is synchronous.
   */
  std::unique_ptr<server_connection_iface> accept(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Stop the persistent reader/writer; all connections become closed. */
  void stop() override;

 private:
  int read_fd_;
  int write_fd_;
  stdio_passthrough_cb passthrough_;
  std::atomic<bool> stopped_{false};
  std::atomic<bool> checked_out_{false};
  std::shared_ptr<stdio_conn_state> state_;
};

} // namespace bdg::bison::rmi::transport
