// MIT License © 2025 Binary Dice Games
/**
 * @file term_transport.hpp
 * @brief RMI transport that frames envelopes as OSC-99 escape sequences over
 *        a pair of raw fds (a `bdg::bison::term::terminal`'s I/O handles, or
 *        a process's own inherited stdio).
 *
 * Wire framing: `ESC ] 99 ; <seq> ; <total> ; <base64(chunk)> BEL`
 * (`\x1b]99;<seq>;<total>;<base64(chunk)>\x07`). Unlike
 * `src/rmi/transport/stdio_transport.hpp`'s `BISON<...>` framing, this is
 * designed to survive being relayed through a real pseudo-console (ConPTY on
 * Windows, a pty on Linux/MSYS2): terminal emulators and ConPTY are built to
 * shepherd OSC (Operating System Command) escape sequences through as atomic
 * units — even ones they don't recognize — rather than mangling arbitrary
 * embedded text, which is what made `BISON<...>` framing unreliable when
 * relayed through ConPTY (see `src/term/ANALYSIS.md`).
 *
 * Every envelope is split into one or more `<seq>;<total>;...` chunks capped
 * at `kMaxOscSequenceBytes` total sequence length, so a single large envelope
 * can never produce an oversized escape sequence that a pseudo-console might
 * mishandle. See `term_transport.cpp` for the chunking and reassembly
 * details, and `FORMAT.md` §5.3 for the full wire format.
 *
 * Non-OSC-99 bytes (the shell/console's actual output, including other,
 * unrelated OSC sequences such as window-title updates) are forwarded
 * verbatim to a caller-supplied passthrough callback, exactly as
 * `stdio_transport` does — this is what keeps the terminal session fully
 * interactive.
 *
 * Reuses `stdio_transport.hpp`'s `stdio_passthrough_cb` type and default
 * passthrough callbacks, and its plain-text connect-time handshake
 * (`START BISON/1.0` / `BISON/1.0 OK` / `STOP BISON/1.0`) verbatim — framing
 * is the only thing this transport does differently.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/transport/stdio_transport.hpp"
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
   * @param handshake_timeout How long `open()` waits for `BISON/1.0 OK`
   *                          before giving up.
   */
  term_client_transport(
      int read_fd,
      int write_fd,
      stdio_passthrough_cb passthrough = stdio_discard_passthrough,
      std::chrono::milliseconds handshake_timeout = kDefaultHandshakeTimeout);
  ~term_client_transport() override;

  /**
   * @brief Starts the background I/O loops, then runs the connect-time
   *        handshake: sends `START BISON/1.0\r\n` and blocks until either
   *        `BISON/1.0 OK\r\n` arrives from the peer or `handshake_timeout`
   *        elapses.
   * @throws std::runtime_error if no `BISON/1.0 OK` arrives in time.
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
   * @brief Sends `STOP BISON/1.0\r\n` (best-effort) then stops the
   *        background I/O loops; pending and future receives return `false`.
   */
  void shutdown() override;

 private:
  std::unique_ptr<term_conn_state> state_;
  std::chrono::milliseconds handshake_timeout_;
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
  term_server_transport(int read_fd, int write_fd, stdio_passthrough_cb passthrough = stdio_print_passthrough);
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
  stdio_passthrough_cb passthrough_;
  std::atomic<bool> stopped_{false};
  bison::synchronized<bool> checked_out_{false};
  std::shared_ptr<term_conn_state> state_;
};

} // namespace bdg::bison::rmi::transport
