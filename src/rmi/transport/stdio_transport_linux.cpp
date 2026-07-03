// MIT License © 2025 Binary Dice Games
/**
 * @file stdio_transport_linux.cpp
 * @brief Linux implementation of the fd-duplication hook `stdio_transport.cpp`
 *        uses so that closing a connection's libuv pipe handles doesn't close
 *        the caller's original fd (see the doc comment on `dup_stdio_fd` in
 *        `stdio_transport.cpp`).
 */
#include "src/rmi/transport/stdio_transport.hpp"

#include <unistd.h>

namespace bdg::bison::rmi::transport {

int dup_stdio_fd(int fd) {
  return dup(fd);
}

} // namespace bdg::bison::rmi::transport
