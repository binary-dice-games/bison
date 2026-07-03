// MIT License © 2025 Binary Dice Games
/**
 * @file pty_write.cpp
 * @brief Portable half of pty_write.hpp. See the header for the rationale;
 *        write_raw() itself is platform-specific
 *        (pty_write_linux.cpp/pty_write_win.cpp).
 */
#include "src/pty/pty_write.hpp"

namespace bdg::bison::pty {

std::string to_crlf(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    if (c == '\n')
      out.push_back('\r');
    out.push_back(c);
  }
  return out;
}

} // namespace bdg::bison::pty
