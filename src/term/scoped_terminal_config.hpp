// MIT License © 2025 Binary Dice Games
/**
 * @file scoped_terminal_config.hpp
 */
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace bdg::bison::term {

/**
 * @brief RAII: Configures a terminal-backed transport's fds for framed RMI
 *        traffic, restoring everything on destruction.
 *
 * Puts `params::read_fd` into raw mode and rewrites `std::cout`/`std::cerr`'s
 * outgoing `'\n'` to `"\r\n"` (see `crlf_output_guard`'s doc comment) for the
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
  /** @brief Opaque platform-specific raw-mode saved-state. Defined in the platform .cpp. */
  struct impl;
  /** @brief Opaque CRLF streambuf state. Platform-independent; defined in scoped_terminal_config.cpp. */
  struct crlf_state;

  using impl_ptr = std::unique_ptr<impl, void (*)(impl*)>;
  using crlf_ptr = std::unique_ptr<crlf_state, void (*)(crlf_state*)>;

  impl_ptr create_state(const params& p);
  void release_state(impl_ptr state);

  crlf_ptr create_crlf_state(const params& p);
  void release_crlf_state(crlf_ptr state);

  params params_;
  impl_ptr impl_;
  crlf_ptr crlf_;
};

} // namespace bdg::bison::term
