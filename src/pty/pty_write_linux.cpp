// MIT License © 2025 Binary Dice Games
/**
 * @file pty_write_linux.cpp
 * @brief Linux implementation of pty_write.hpp's write_raw().
 */
#include "src/pty/pty_write.hpp"

#include <cerrno>
#include <unistd.h>

namespace bdg::bison::pty {

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
