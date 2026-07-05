// MIT License © 2025 Binary Dice Games
/**
 * @file fd_dup_posix.cpp
 * @brief Implementation of fd_dup.hpp (Linux and MSYS2).
 */
#include "src/rmi/transport/fd_dup.hpp"

#include <unistd.h>

namespace bdg::bison::rmi::transport {

int dup_fd(int fd) {
  return dup(fd);
}

} // namespace bdg::bison::rmi::transport
