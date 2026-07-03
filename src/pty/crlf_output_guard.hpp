// MIT License © 2025 Binary Dice Games
/**
 * @file crlf_output_guard.hpp
 * @brief RAII helper that rewrites `std::cout`/`std::cerr` output so it
 *        displays correctly on a raw-mode terminal.
 */
#pragma once

#include <functional>
#include <memory>
#include <string_view>

namespace bdg::bison::pty {

/**
 * @brief RAII: rewrites `std::cout`/`std::cerr`'s outgoing `'\n'` to
 *        `"\r\n"` for its lifetime, restoring the original streambufs on
 *        destruction.
 *
 * `raw_mode_guard` (client side) and `pty_process` (server side, which puts
 * the *operator's own* real terminal in raw mode for `pump_loop()` — see
 * `src/pty/DESIGN.md`) both disable `OPOST`/`ONLCR` on a terminal's termios
 * so that outgoing `BISON:` frame bytes aren't corrupted by the kernel
 * inserting stray `\r`s before `\n`s. But that termios setting is global to
 * the fd/tty, not scoped to frame writes — it also strips the `\r` from
 * every *other* byte written there, including plain program output. Real
 * terminals don't auto-return on a bare `\n` (that translation is exactly
 * what `ONLCR` was doing), so without this, `--pty` output stairsteps down
 * and to the right instead of starting each line at the left margin. This
 * class reimplements just that translation in software, for
 * `std::cout`/`std::cerr` specifically — the streams `--pty` programs in
 * this codebase print through.
 *
 * Platform-independent (pure `std::streambuf`), so — unlike `raw_mode_guard`
 * and `pty_process` — this has no Linux/Windows split.
 */
class crlf_output_guard {
 public:
  /**
   * @brief Writes translated bytes straight to `std::cout`/`std::cerr`'s
   *        current underlying streambuf (i.e. the real fd).
   *
   * Only safe when nothing else writes to that fd concurrently from another
   * thread — a `--pty` client that also writes there via a transport's own
   * background writer (e.g. for local keystroke echo) must use the `sink`
   * constructor instead, so every write funnels through one place. Two
   * independent writers racing on the same fd is exactly the kind of bug
   * this codebase has been chasing throughout `--pty` support; don't
   * reintroduce it here.
   */
  crlf_output_guard();

  /**
   * @brief Forwards translated bytes to @p sink instead of writing to the
   *        real fd directly.
   *
   * Use this when something else (typically a transport's synchronized
   * writer queue) must be the sole writer of the underlying fd. @p sink is
   * called from whatever thread performs the `std::cout`/`std::cerr` write
   * — it must be safe to call from there, and ideally non-blocking, since
   * it runs inline inside the stream's `<<` call.
   */
  explicit crlf_output_guard(std::function<void(std::string_view)> sink);

  ~crlf_output_guard();

  crlf_output_guard(const crlf_output_guard&) = delete;
  crlf_output_guard& operator=(const crlf_output_guard&) = delete;
  crlf_output_guard(crlf_output_guard&&) = delete;
  crlf_output_guard& operator=(crlf_output_guard&&) = delete;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace bdg::bison::pty
