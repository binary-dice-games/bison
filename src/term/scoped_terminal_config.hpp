// MIT License © 2025 Binary Dice Games
/**
 * @file scoped_terminal_config.hpp
 */
#pragma once

#include "src/bison/bison_sync.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace bdg::bison::term {

/**
 * @brief RAII: makes `params::read_fd`/`params::write_fd` safe for a caller
 *        to treat as plain, protocol-unaware terminal I/O, even though a
 *        transport elsewhere is multiplexing framed traffic over the same
 *        underlying fds.
 *
 * Puts `params::read_fd` into raw mode for the object's lifetime, same as
 * before. In addition, `read_fd`/`write_fd` (and stderr) are redirected
 * through internal pipes for the object's lifetime, and the *real* fds they
 * used to refer to are exposed via `upstream_read_fd()`/`upstream_write_fd()`
 * — intended for a transport (e.g. a term-transport) to be constructed from,
 * so it becomes the sole reader/writer of the real fds:
 *
 * - Inbound: whatever the transport reads from `upstream_read_fd()` that
 *   isn't part of a framed envelope (its "pass-through" bytes) should be
 *   handed to `on_passthrough()`, which makes it show up as plain data on
 *   `params::read_fd` (e.g. readable via `std::cin`) — the caller never
 *   sees or has to parse the transport's framing.
 * - Outbound: anything written to `params::write_fd` or stderr (via
 *   `std::cout`/`std::cerr`, C stdio, or a raw `write()` — every writer is
 *   covered, since this is fd-level redirection) is CRLF-translated and
 *   forwarded to the channel installed with `set_output_channel()`, instead
 *   of racing the transport's own writes to the real fd. Until
 *   `set_output_channel()` is called, writes go straight to
 *   `upstream_write_fd()` (safe: nothing else is writing to it yet at that
 *   point).
 */
class scoped_terminal_config {
 public:
  struct params {
    int read_fd{};
    int write_fd{};
  };

  explicit scoped_terminal_config(params&& p);

  ~scoped_terminal_config();

  scoped_terminal_config(const scoped_terminal_config&) = delete;
  scoped_terminal_config& operator=(const scoped_terminal_config&) = delete;
  scoped_terminal_config(scoped_terminal_config&&) = delete;
  scoped_terminal_config& operator=(scoped_terminal_config&&) = delete;

  /** @brief Real fd `params::read_fd` referred to before it was redirected. */
  int upstream_read_fd() const;

  /** @brief Real fd `params::write_fd` referred to before it was redirected. */
  int upstream_write_fd() const;

  /**
   * @brief Feed one chunk of non-framed bytes read from `upstream_read_fd()`
   *        so it appears as plain data on `params::read_fd`.
   *
   * Raw mode (needed so byte-level protocol framing sees untouched bytes)
   * disables the terminal's own line editing, so this implements just
   * enough of it in software for a line-based reader like
   * `std::getline()` to behave normally:
   * - A bare `'\r'` (what a real terminal sends for the Enter key, now that
   *   raw mode disables the usual kernel/console CR->LF translation) is
   *   rewritten to `'\n'`; a `"\r\n"` pair collapses to one `'\n'`.
   * - Bytes are only delivered to `params::read_fd` one whole line at a
   *   time, once `'\n'` is seen — matching what canonical (cooked) mode
   *   would have buffered for a caller anyway.
   * - Backspace/Delete (`0x08`/`0x7f`) erases the last buffered,
   *   not-yet-delivered character instead of being delivered literally.
   * - Every byte is also echoed back through `set_output_channel()`'s
   *   channel (CRLF-translated, so it displays correctly) — raw mode also
   *   disables the terminal's own local echo, so without this the operator
   *   would see nothing as they type.
   *
   * @param chunk Bytes to deliver; an empty chunk signals the upstream read
   *              side closed (EOF) — `params::read_fd` reports EOF once any
   *              already-delivered bytes are drained.
   */
  void on_passthrough(std::string_view chunk);

  /**
   * @brief Alternative to `on_passthrough()`'s default delivery: installs a
   *        sink that receives every pass-through chunk immediately,
   *        verbatim -- no software line buffering, no backspace handling,
   *        and no automatic echo. Once installed, `on_passthrough()` stops
   *        delivering to `params::read_fd`'s pipe (and stops echoing)
   *        entirely; the sink owner takes over both responsibilities.
   *
   * For a caller that wants to react to individual keystrokes itself (e.g.
   * arrow-key history in a line editor) instead of waiting for whole,
   * already-backspace-applied lines on `params::read_fd`. Mirrors
   * `on_passthrough()`'s own empty-chunk EOF signal -- the sink is called
   * with an empty chunk when the upstream read side closes.
   *
   * Call once, before the first `on_passthrough()` call.
   *
   * @param sink Receives each pass-through chunk as it arrives.
   */
  void set_raw_input_sink(std::function<void(std::string_view)> sink);

  /**
   * @brief Write @p chunk verbatim to @p fd, retrying on partial writes.
   *
   * An alternative to `on_passthrough()` for feeding a target that already
   * provides its own real terminal line discipline -- e.g. a spawned
   * `term::terminal`'s pty running an interactive shell with readline.
   * Unlike `on_passthrough()`, this delivers bytes immediately and
   * unbuffered, with no software line reconstruction and no separate echo:
   * a real pty naturally echoes what it receives itself, and readline needs
   * every keystroke live (not batched until a newline) to handle history,
   * arrow keys, and line editing correctly. Feeding such a target through
   * `on_passthrough()` instead breaks exactly that -- input arrives batched
   * and delayed, and gets echoed twice (once by `on_passthrough()`, once by
   * the target's own pty).
   *
   * Static: does not touch any state owned by a `scoped_terminal_config`
   * instance -- @p fd is any target fd, not necessarily one this class
   * manages (e.g. another spawned terminal's own write handle).
   *
   * @param fd    Fd to write to.
   * @param chunk Bytes to write.
   */
  static void on_terminal_passthrough(int fd, std::string_view chunk);

  /**
   * @brief Install the channel that CRLF-translated writes to
   *        `params::write_fd`/stderr are forwarded to, instead of
   *        `upstream_write_fd()` directly. Call once, right after
   *        constructing whatever owns `upstream_write_fd()`.
   */
  void set_output_channel(std::function<void(std::string_view)> sink);

  /**
   * @brief Stops and joins the background thread that delivers output to
   *        the channel installed by `set_output_channel()`, then restores
   *        `params::write_fd`/stderr to write straight to
   *        `upstream_write_fd()`. Idempotent — the destructor calls this
   *        too, so it's a no-op if already stopped.
   *
   * The channel is typically a callback capturing a pointer back into
   * whatever owns `upstream_write_fd()` (e.g. a transport's `send()`). That
   * object's destructor must call this *before* tearing down anything the
   * channel touches — otherwise the pump thread could still be mid-flight
   * delivering already-buffered output through a now-dangling pointer after
   * that object is gone. See `term_client_transport`'s `before_destroy`
   * constructor parameter, which exists for exactly this purpose.
   */
  void stop_output_pump();

 private:
  /** @brief Opaque platform-specific state. Defined in the platform .cpp. */
  struct impl;

  using impl_ptr = std::unique_ptr<impl, void (*)(impl*)>;

  impl_ptr create_state(const params& p);
  void release_state(impl_ptr state);

  /** @brief Background thread body: drains the output pipe, CRLF-translates, forwards. */
  static void run_output_pump(impl* state);

  /**
   * @brief Software line discipline shared by both platforms' on_passthrough():
   *        CRLF-normalizes @p chunk, applies backspace/delete against
   *        @p line_buffer, and splits the result into @p to_echo (every
   *        processed byte, for local echo) and @p to_deliver (only whole,
   *        newline-terminated lines drained out of @p line_buffer).
   * @param last_was_cr In/out CRLF-pair-splitting state; see on_passthrough()'s
   *                     doc comment.
   * @param line_buffer In/out buffer of the current, not-yet-delivered line.
   */
  static void process_passthrough_chunk(
      std::string_view chunk,
      bool& last_was_cr,
      std::string& line_buffer,
      std::string& to_echo,
      std::string& to_deliver);

  params params_;
  impl_ptr impl_;
  bison::synchronized<std::function<void(std::string_view)>> raw_input_sink_;
};

} // namespace bdg::bison::term
