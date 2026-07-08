// MIT License © 2025 Binary Dice Games
/**
 * @file term_transport.hpp
 * @brief RMI transport that frames envelopes over a pair of raw fds (a
 *        `bdg::bison::term::terminal`'s I/O handles, or a process's own
 *        inherited stdio) — using a *different* wire format in each
 *        direction, because a pseudo-console's two pipes aren't symmetric.
 *
 * Client -> server envelopes: `ESC ] 99 ; <seq> ; <total> ; <base64(chunk)>
 * BEL` (`\x1b]99;<seq>;<total>;<base64(chunk)>\x07`), split into one or more
 * `<seq>;<total>;...` chunks capped at `kMaxOscSequenceBytes` total sequence
 * length. This direction is relayed client-stdout -> ConPTY *output* pipe
 * (Windows) / pty (Linux/MSYS2) -> server-read, and terminal emulators and
 * ConPTY are built to shepherd OSC (Operating System Command) escape
 * sequences through as atomic units — even ones they don't recognize —
 * making this format safe to relay unmangled.
 *
 * Server -> client envelopes: This direction is relayed server-write ->
 * ConPTY's *input* pipe -> client-stdin, and that pipe is not a passthrough
 * channel: ConPTY runs it through a VT *input* state machine to synthesize
 * keyboard events for the child process, which only recognizes a small
 * fixed table of real input sequences and has no "relay anything
 * unrecognized" contract — an OSC-99 sequence written there is absorbed by
 * the parser and never reaches the client as literal bytes. Plain marker
 * text with no ESC byte is what survives this direction instead (this
 * asymmetry, and the same fix, is documented in `src/term/ANALYSIS.md` §2).
 *
 * See `term_transport.cpp`'s `term_role` for how each side picks its
 * send/receive format, the chunking and reassembly details, and `FORMAT.md`
 * §5.3 for the full wire format.
 *
 * Bytes that don't match either sequence (the shell/console's actual
 * output, including other, unrelated OSC sequences such as window-title
 * updates) are forwarded verbatim to a caller-supplied passthrough
 * callback — this is what keeps the terminal session fully interactive.
 *
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string_view>

namespace bdg::bison::rmi::transport {

/** @brief Opaque libuv-backed connection state. Defined in the .cpp. */
struct term_conn_state;

/**
 * @brief Callback invoked with each chunk of non-`BISON<...>` bytes, in arrival
 *        order. Called once more with an empty chunk when the read side
 *        closes (EOF or error), as a stream-closed signal; this never
 *        happens mid-stream otherwise since empty reads are not forwarded.
 */
using term_passthrough_cb = std::function<void(std::string_view chunk)>;

/** @brief Default passthrough: write the chunk verbatim to stdout. */
void term_print_passthrough(std::string_view chunk);

/** @brief Default passthrough: discard the chunk. */
void term_discard_passthrough(std::string_view chunk);

/** @brief Default wait for the first confirming frame after open(); see is_connected()'s doc comment. */
inline constexpr std::chrono::milliseconds kDefaultHandshakeTimeout{1000};

/**
 * @brief Cap, in bytes, on one whole OSC-99 escape sequence
 *        (`\x1b]99;...\x07`, inclusive), so a pseudo-console is never asked
 *        to relay an arbitrarily large control sequence. Envelopes larger
 *        than this budget allows are split across multiple chunks; see
 *        term_transport.cpp.
 */
inline constexpr size_t kMaxOscSequenceBytes = 4096;

// ── Client-side transport ─────────────────────────────────────────────────────

/**
 * @brief Client transport that wraps a pair of already-open fds using
 *        OSC-99 chunked framing.
 *
 * Typically constructed from a `bdg::bison::term::terminal`'s
 * `read_handle()`/`write_handle()` (server side, driving the spawned pty),
 * or from a process's own inherited stdio (client side, the process running
 * *inside* the spawned terminal).
 */
class term_client_transport : public client_transport_iface {
 public:
  /**
   * @param read_fd           Fd to read peer bytes from.
   * @param write_fd          Fd to write frames and pass-through bytes to.
   * @param passthrough       Called with non-OSC-99 bytes read from `read_fd`.
   * @param handshake_timeout How long, after `open()`, `is_connected()`
   *                          keeps reporting `true` while no frame has yet
   *                          been received from the peer.
   * @param before_destroy    If set, called at the start of both
   *                          `shutdown()` and the destructor (whichever
   *                          runs first — safe to call more than once),
   *                          before any internal state is torn down or
   *                          marked closed. Lets a caller that handed this
   *                          transport a callback capturing a pointer back
   *                          to itself (e.g.
   *                          `scoped_terminal_config::set_output_channel()`)
   *                          stop using — and flush anything already
   *                          buffered through — that pointer before it
   *                          dangles, instead of racing this transport's
   *                          own teardown against a background thread that
   *                          might still be mid-flight calling `send()`.
   */
  term_client_transport(
      int read_fd,
      int write_fd,
      term_passthrough_cb passthrough = term_discard_passthrough,
      std::chrono::milliseconds handshake_timeout = kDefaultHandshakeTimeout,
      std::function<void()> before_destroy = nullptr);
  ~term_client_transport() override;

  /**
   * @brief Arms the `is_connected()` deadline (see its doc comment). The
   *        background I/O loops are already running (started at
   *        construction time) — real connect confirmation is the caller's
   *        `OP_CONNECT` request/response round trip over the now-live
   *        OSC-99/marker channel, not anything done here.
   */
  void open(bison::dynamic params) override;

  /** @brief Split into OSC-99 chunks (if needed) and write them. */
  void send(bison::buffer frame) override;

  /** @brief Write @p bytes verbatim, with no OSC-99 framing (e.g. local keystroke echo). */
  void send(std::string_view bytes);

  /**
   * @brief Wait for the next fully-reassembled OSC-99 envelope.
   * @param frame   Output frame buffer.
   * @param timeout Maximum wait before returning `false`.
   * @return `true` on success; `false` on timeout, EOF, or shutdown.
   */
  bool receive(bison::buffer& frame, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /**
   * @brief Stops the background I/O loops; pending and future receives
   *        return `false`. The caller (`client::disconnect()`) sends
   *        `OP_DISCONNECT` over the normal frame path before calling this.
   */
  void shutdown() override;

  /**
   * @brief `false` once the peer's read side has closed, or once
   *        `handshake_timeout` (passed to the constructor) has elapsed
   *        since `open()` without any frame ever having been received from
   *        the peer; `true` otherwise. This is what gives a `connect()`
   *        against a dead/nonexistent peer a bounded failure instead of
   *        hanging: `client::worker_loop()` treats a `false` result here as
   *        a transport failure and fails the pending `OP_CONNECT` future.
   */
  bool is_connected() const override;

 private:
  std::unique_ptr<term_conn_state> state_;
  std::chrono::milliseconds handshake_timeout_;
  std::chrono::steady_clock::time_point connect_deadline_;
  std::function<void()> before_destroy_;
};

// ── Server-side connection ────────────────────────────────────────────────────

/**
 * @brief Server-side connection backed by a pair of already-open fds.
 *
 * Produced by `term_server_transport::accept()`. Wraps a *shared* reference
 * to the transport's one persistent `term_conn_state`, mirroring
 * `stdio_server_connection` — see its doc comment for the rationale (a
 * spawned pty session must keep being pumped independent of any one RMI
 * client connection's lifetime).
 */
class term_server_connection : public server_connection_iface {
 public:
  term_server_connection(std::shared_ptr<term_conn_state> state, std::function<void()> on_close);
  ~term_server_connection() override;

  void send(bison::buffer frame) override;
  bool receive(bison::buffer& frame, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;
  void close() override;
  bool is_closed() const override;

 private:
  std::shared_ptr<term_conn_state> state_;
  std::function<void()> on_close_;
  std::atomic<bool> closed_{false};
};

// ── Server-side transport (persistent reader, sequential connections) ─────────

/**
 * @brief Server transport over a pair of already-open fds (typically a
 *        `bdg::bison::term::terminal`'s I/O handles), supporting sequential
 *        (not concurrent) connections. Mirrors `stdio_server_transport`.
 */
class term_server_transport : public server_transport_iface {
 public:
  /**
   * @param read_fd      Fd to read peer bytes from.
   * @param write_fd     Fd to write frames and pass-through bytes to.
   * @param passthrough  Called with non-OSC-99 bytes read from `read_fd`.
   */
  term_server_transport(int read_fd, int write_fd, term_passthrough_cb passthrough = term_print_passthrough);
  ~term_server_transport() override;

  term_server_transport(const term_server_transport&) = delete;
  term_server_transport& operator=(const term_server_transport&) = delete;

  void start(bison::dynamic params) override;
  std::unique_ptr<server_connection_iface> accept(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;
  void stop() override;

 private:
  int read_fd_;
  int write_fd_;
  term_passthrough_cb passthrough_;
  std::atomic<bool> stopped_{false};
  bison::synchronized<bool> checked_out_{false};
  std::shared_ptr<term_conn_state> state_;
};

} // namespace bdg::bison::rmi::transport
