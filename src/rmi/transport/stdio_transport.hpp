// MIT License © 2025 Binary Dice Games
/**
 * @file stdio_transport.hpp
 * @brief RMI transport that frames envelopes as base64 sequences over a pair
 *        of raw fds (typically stdin/stdout, or a pty master fd).
 *
 * Wire framing: `BISON<base64(frame)>`. Bytes that are not part of a
 * confirmed `BISON<...>` frame are forwarded verbatim, as soon as they
 * arrive, to a caller-supplied passthrough callback — this is what keeps a
 * pty session fully interactive (see `src/pty/DESIGN.md`) instead of
 * behaving like a line-buffered reader. See `FORMAT.md` for the full
 * framing contract.
 *
 * `stdio_client_transport::open()` also runs a connect-time handshake (a
 * plain-text `START BISON/1.0` / `BISON/1.0 OK` exchange — see `open()`'s
 * doc comment) so that connecting with no peer on the other end of the fds
 * fails with a timeout instead of hanging forever waiting for a
 * `BISON<...>` frame that will never arrive.
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
 * @brief Callback invoked with each chunk of non-`BISON<...>` bytes, in arrival
 *        order. Called once more with an empty chunk when the read side
 *        closes (EOF or error), as a stream-closed signal; this never
 *        happens mid-stream otherwise since empty reads are not forwarded.
 */
using stdio_passthrough_cb = std::function<void(std::string_view chunk)>;

/** @brief Default passthrough: write the chunk verbatim to stdout. */
void stdio_print_passthrough(std::string_view chunk);

/** @brief Default passthrough: discard the chunk. */
void stdio_discard_passthrough(std::string_view chunk);

/** @brief Default wait for open()'s connect-time handshake; see its doc comment. */
inline constexpr std::chrono::milliseconds kDefaultHandshakeTimeout{5000};

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
   * @param read_fd           Fd to read peer bytes from.
   * @param write_fd          Fd to write frames and pass-through bytes to.
   * @param passthrough       Called with non-`BISON<...>` bytes read from `read_fd`.
   * @param handshake_timeout How long `open()` waits for `BISON/1.0 OK`
   *                          before giving up — see `open()`'s doc comment.
   *                          Exposed mainly so tests don't have to wait out
   *                          the real default; production callers should
   *                          leave it at the default.
   */
  stdio_client_transport(
      int read_fd,
      int write_fd,
      stdio_passthrough_cb passthrough = stdio_discard_passthrough,
      std::chrono::milliseconds handshake_timeout = kDefaultHandshakeTimeout);
  ~stdio_client_transport() override;

  /**
   * @brief Starts the background I/O loops, then runs the connect-time
   *        handshake: sends `START BISON/1.0\n` and blocks until either
   *        `BISON/1.0 OK\n` arrives from the peer or `handshake_timeout`
   *        (constructor parameter) elapses.
   *
   * @throws std::runtime_error if no `BISON/1.0 OK` arrives in time.
   */
  void open(bison::dynamic params) override;

  /** @brief Base64-wrap and write one frame as a `BISON<...>` sequence. */
  void send(bison::buffer frame) override;

  /** @brief Write @p bytes to `write_fd` verbatim, with no `BISON<...>` framing. */
  void send(std::string_view bytes);

  /**
   * @brief Wait for the next decoded `BISON<...>` frame.
   * @param frame   Output frame buffer.
   * @param timeout Maximum wait before returning `false`.
   * @return `true` on success; `false` on timeout, EOF, or shutdown.
   */
  bool receive(bison::buffer& frame, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /**
   * @brief Sends `STOP BISON/1.0\n` (best-effort — errors are ignored, this
   *        is advisory) then stops the background I/O loops; pending and
   *        future receives return `false`.
   */
  void shutdown() override;

 private:
  std::unique_ptr<stdio_conn_state> state_;
  std::chrono::milliseconds handshake_timeout_;
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

  /** @brief Base64-wrap and write one frame as a `BISON<...>` sequence. */
  void send(bison::buffer frame) override;

  /**
   * @brief Wait for the next decoded `BISON<...>` frame.
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
 * one is outstanding, further calls block (up to the caller's `timeout`) on
 * the checked-out connection's `close()` running, at which point the next
 * `accept()` call succeeds again. Blocking here (rather than returning
 * `nullptr` immediately) matters because `server::accept_loop()` polls
 * `accept()` in a tight `while` loop with no sleep of its own — it relies on
 * `accept()` itself doing the waiting, the way `socket_server_transport`'s
 * and `named_pipe_server_transport`'s condvar-backed implementations do. This
 * lets an operator run the RMI client, exit back to the shell, and run it
 * again — each run is its own connection and its own `client_worker` session
 * server-side — without restarting the pty or the server.
 *
 * Also answers `stdio_client_transport::open()`'s connect-time handshake:
 * whenever `START BISON/1.0\n` shows up anywhere in the byte stream, this
 * replies with `BISON/1.0 OK\n` — independent of `accept()`/connection
 * state, so it keeps working across every reconnect for the transport's
 * whole lifetime. Unlike the client side, this isn't gated behind a
 * connection-setup step: the handshake text is left in the normal
 * passthrough stream (visible to whatever's watching `--pty` output) rather
 * than being consumed/hidden, since nothing here needs to treat it as
 * anything other than an informational line to react to.
 */
class stdio_server_transport : public server_transport_iface {
 public:
  /**
   * @param read_fd      Fd to read peer bytes from.
   * @param write_fd     Fd to write frames and pass-through bytes to. May
   *                     equal `read_fd` (e.g. a pty master fd).
   * @param passthrough  Called with non-`BISON<...>` bytes read from `read_fd`.
   */
  stdio_server_transport(int read_fd, int write_fd, stdio_passthrough_cb passthrough = stdio_print_passthrough);
  ~stdio_server_transport() override;

  stdio_server_transport(const stdio_server_transport&) = delete;
  stdio_server_transport& operator=(const stdio_server_transport&) = delete;

  /** @brief Opens the persistent reader/writer over `read_fd_`/`write_fd_`. */
  void start(bison::dynamic params) override;

  /**
   * @brief Return a new connection once none is currently checked out.
   * @param timeout  How long to block waiting for a previously checked-out
   *                  connection to close before giving up.
   * @return A new connection, or `nullptr` on timeout (including after
   *         `stop()`).
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
  bison::synchronized<bool> checked_out_{false};
  std::shared_ptr<stdio_conn_state> state_;
};

} // namespace bdg::bison::rmi::transport
