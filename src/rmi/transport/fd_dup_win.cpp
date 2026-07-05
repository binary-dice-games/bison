// MIT License © 2025 Binary Dice Games
/**
 * @file fd_dup_win.cpp
 * @brief Implementation of fd_dup.hpp (native Windows).
 */
#include "src/rmi/transport/fd_dup.hpp"

#include <io.h>

namespace bdg::bison::rmi::transport {

int dup_fd(int fd) {
  return _dup(fd);
}

} // namespace bdg::bison::rmi::transport
