// MIT License © 2025 Binary Dice Games
/**
 * @file pty_write.cpp
 * @brief Implementation of pty_write.hpp (Linux and MSYS2).
 */
#include "src/pty/pty_write.hpp"

#include <cerrno>
#include <unistd.h>

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

void write_raw(int fd, std::string_view bytes) {
  size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t n = write(fd, bytes.data() + written, bytes.size() - written);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return;
    }
    if (n == 0)
      return;
    written += static_cast<size_t>(n);
  }
}

} // namespace bdg::bison::pty
