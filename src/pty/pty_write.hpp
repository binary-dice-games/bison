// MIT License © 2025 Binary Dice Games
/**
 * @file pty_write.hpp
 * @brief Raw fd write for one-off `--pty` status messages that can't safely
 *        go through `crlf_output_guard`'s `std::cout` redirection.
 */
#pragma once

#include <string>
#include <string_view>

namespace bdg::bison::pty {

/**
 * @brief Rewrites `'\n'` to `"\r\n"` in @p text, returning the result.
 *
 * Pure string transform, no OS calls — pair with `write_raw()` below.
 */
std::string to_crlf(std::string_view text);

/**
 * @brief Write all of @p bytes to @p fd, looping over partial writes.
 *
 * Best-effort: write errors are ignored, matching how a status message
 * failing to display isn't worth crashing the process over.
 *
 * Exists for `server_app`'s `--pty` status messages
 * (`on_listening()`/`on_verbose_trace()`) and the equivalent lines in
 * `examples/rmi_server_example.cpp`. Those can't use `crlf_output_guard`
 * the way `client_app` does: `stdio_server_transport`'s passthrough
 * callback (`stdio_print_passthrough`) *also* writes to `std::cout`, from a
 * different thread, to forward pty-master bytes (shell output, or a
 * `--pty` client's own already-`\r\n`-corrected text) **verbatim** —
 * redirecting `std::cout`'s streambuf would (a) double the `\r` on
 * anything forwarded that way, corrupting it, and (b) race that thread's
 * writes, since swapping a stream's streambuf concurrently with another
 * thread's `<<` on the same stream isn't safe. Writing directly to the fd
 * here, bypassing `std::cout` entirely, sidesteps both problems — see
 * `src/pty/DESIGN.md`'s Design Decisions for the full writeup.
 *
 * Platform-specific (`pty_write_linux.cpp`/`pty_write_win.cpp`) only for
 * this function; `to_crlf()` above is portable.
 */
void write_raw(int fd, std::string_view bytes);

} // namespace bdg::bison::pty
