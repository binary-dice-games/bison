// MIT License © 2025 Binary Dice Games
/**
 * @file pty_write.cpp
 * @brief Implementation of pty_write.hpp's portable pieces. `write_raw()` is
 *        implemented per-platform in `pty_write_raw_posix.cpp`/
 *        `pty_write_raw_win.cpp`.
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
