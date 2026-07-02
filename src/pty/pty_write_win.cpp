// MIT License © 2025 Binary Dice Games
/**
 * @file pty_write_win.cpp
 * @brief Windows implementation of pty_write.hpp's write_raw().
 */
#include "src/pty/pty_write.hpp"

#include <io.h>

namespace bdg::bison::pty {

void write_raw(int fd, std::string_view bytes) {
  size_t written = 0;
  while (written < bytes.size()) {
    const int n = _write(fd, bytes.data() + written, static_cast<unsigned int>(bytes.size() - written));
    if (n <= 0)
      return;
    written += static_cast<size_t>(n);
  }
}

} // namespace bdg::bison::pty
