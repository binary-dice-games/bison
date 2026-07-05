// MIT License © 2025 Binary Dice Games
/**
 * @file socket_dup_win.cpp
 * @brief Implementation of socket_dup.hpp (native Windows).
 *
 * Winsock `SOCKET` handles aren't plain fds, so `dup()` doesn't apply.
 * `WSADuplicateSocketW()` produces a `WSAPROTOCOL_INFOW` blob describing the
 * socket, which `WSASocketW(..., FROM_PROTOCOL_INFO, ...)` turns into a new
 * socket handle in the target process — here, the same process, since this
 * is only used to hand a socket off from a temporary uv_tcp_t to a fresh one
 * on another loop.
 */
#include "src/rmi/transport/socket_dup.hpp"

#include <winsock2.h>

namespace bdg::bison::rmi::transport {

uv_os_sock_t duplicate_tcp_socket(uv_tcp_t* handle) {
  uv_os_fd_t fd{};
  if (uv_fileno(reinterpret_cast<uv_handle_t*>(handle), &fd) != 0)
    return INVALID_SOCKET;

  const SOCKET sock = reinterpret_cast<SOCKET>(fd);
  WSAPROTOCOL_INFOW info{};
  if (WSADuplicateSocketW(sock, GetCurrentProcessId(), &info) != 0)
    return INVALID_SOCKET;

  return WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0, 0);
}

} // namespace bdg::bison::rmi::transport
