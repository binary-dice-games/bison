// MIT License © 2025 Binary Dice Games
/**
 * @file socket_transport_linux.cpp
 * @brief Linux implementation of the socket-duplication hook used to move
 *        an accepted TCP connection onto its own uv_loop_t.
 */
#include "src/rmi/transport/socket_transport.hpp"

#include <unistd.h>

#include <uv.h>

namespace bdg::bison::rmi::transport {

uv_os_sock_t duplicate_tcp_socket(uv_tcp_t* handle) {
  uv_os_fd_t fd{};
  if (uv_fileno(reinterpret_cast<uv_handle_t*>(handle), &fd) != 0)
    return -1;
  return dup(fd);
}

} // namespace bdg::bison::rmi::transport
