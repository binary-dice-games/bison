// MIT License © 2025 Binary Dice Games
/**
 * @file scoped_terminal_config.hpp
 */
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace bdg::bison::term {

/**
 * @brief RAII: combines `pty::raw_mode_guard` and `pty::crlf_output_guard`
 *        into a single type that configures a terminal-backed transport's
 *        fds for framed RMI traffic, restoring everything on destruction.
 *
 * Puts `params::read_fd` into raw mode (see `raw_mode_guard`'s doc comment
 * for the full rationale) and rewrites `std::cout`/`std::cerr`'s outgoing
 * `'\n'` to `"\r\n"` (see `crlf_output_guard`'s doc comment) for the
 * object's lifetime. If `params::sink` is set, translated CRLF output is
 * forwarded there instead of being written to the real streambuf directly
 * — use this when something else (e.g. a transport's synchronized writer)
 * must be the sole writer of `params::write_fd`.
 */
class scoped_terminal_config {
 public:
  struct params {
    int read_fd{};
    int write_fd{};
    std::function<void(std::string_view)> sink{};
  };

  explicit scoped_terminal_config(params&& p);

  ~scoped_terminal_config();

  scoped_terminal_config(const scoped_terminal_config&) = delete;
  scoped_terminal_config& operator=(const scoped_terminal_config&) = delete;
  scoped_terminal_config(scoped_terminal_config&&) = delete;
  scoped_terminal_config& operator=(scoped_terminal_config&&) = delete;

 private:
  /** @brief Opaque platform-specific raw-mode saved-state. Defined per-platform in the .cpp. */
  struct impl;
  /** @brief Opaque CRLF streambuf state. Platform-independent; defined in scoped_terminal_config.cpp. */
  struct crlf_state;

  // Raw (not unique_ptr) pimpl pointers: impl/crlf_state are each only
  // complete in one .cpp file, and this class's ctor/dtor live in the
  // common scoped_terminal_config.cpp, which sees neither definition. A
  // unique_ptr member's destructor requires its pointee complete wherever
  // it's instantiated (including implicitly, at the end of this class's
  // own destructor) -- raw pointers sidestep that, since create_state/
  // release_state/create_crlf_state/release_crlf_state (the only places
  // that actually `new`/`delete` them) are defined alongside each type.
  impl* create_state(const params& p);
  void release_state(impl* state);

  crlf_state* create_crlf_state(const params& p);
  void release_crlf_state(crlf_state* state);

  params params_;
  impl* impl_;
  crlf_state* crlf_;
};

} // namespace bdg::bison::term
