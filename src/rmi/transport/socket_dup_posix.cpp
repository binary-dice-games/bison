// MIT License © 2025 Binary Dice Games
/**
 * @file socket_dup_posix.cpp
 * @brief Implementation of socket_dup.hpp (Linux and MSYS2).
 */
#include "src/rmi/transport/socket_dup.hpp"

#include <unistd.h>

namespace bdg::bison::rmi::transport {

uv_os_sock_t duplicate_tcp_socket(uv_tcp_t* handle) {
  uv_os_fd_t fd{};
  if (uv_fileno(reinterpret_cast<uv_handle_t*>(handle), &fd) != 0)
    return -1;
  return dup(fd);
}

} // namespace bdg::bison::rmi::transport
