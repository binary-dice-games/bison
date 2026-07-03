// MIT License © 2025 Binary Dice Games
/**
 * @file socket_transport_win.cpp
 * @brief Windows implementation of the socket-duplication hook used to move
 *        an accepted TCP connection onto its own uv_loop_t.
 */
#include "src/rmi/transport/socket_transport.hpp"

#include <winsock2.h>

#include <windows.h>

#include <uv.h>

namespace bdg::bison::rmi::transport {

uv_os_sock_t duplicate_tcp_socket(uv_tcp_t* handle) {
  uv_os_fd_t fd{};
  if (uv_fileno(reinterpret_cast<uv_handle_t*>(handle), &fd) != 0)
    return INVALID_SOCKET;

  const auto sock = reinterpret_cast<uv_os_sock_t>(fd);

  WSAPROTOCOL_INFOW info{};
  if (WSADuplicateSocketW(sock, GetCurrentProcessId(), &info) != 0)
    return INVALID_SOCKET;

  return WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0, WSA_FLAG_OVERLAPPED);
}

} // namespace bdg::bison::rmi::transport
