// MIT License © 2025 Binary Dice Games
/**
 * @file tcp_socket_util.hpp
 * @brief Shared TCP socket helpers for libuv-backed transports.
 */
#pragma once

#include <uv.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#include <winsock2.h>
#else
#include <unistd.h>
#endif

namespace bdg::bison::rmi::transport {

/**
 * @brief Duplicate the OS socket underlying an open `uv_tcp_t` handle.
 *
 * A plain `dup()` works for a POSIX socket fd but not for a Winsock
 * `SOCKET`, which requires `WSADuplicateSocket`. Used by the accept path of
 * both `socket_server_transport` and `tls_socket_server_transport` to hand
 * an accepted connection off from the listener's `uv_loop_t` to a fresh
 * `uv_tcp_t` on the connection's own dedicated loop (see
 * `src/rmi/DESIGN.md` §2 for why the hand-off is needed).
 *
 * @return The duplicated socket, or `INVALID_SOCKET`/`-1` on failure.
 */
inline uv_os_sock_t duplicate_tcp_socket(uv_tcp_t* handle) {
  uv_os_fd_t fd{};

#if defined(_WIN32) || defined(__CYGWIN__)
  if (uv_fileno(reinterpret_cast<uv_handle_t*>(handle), &fd) != 0)
    return INVALID_SOCKET;

  // Winsock `SOCKET` handles aren't plain fds, so `dup()` doesn't apply.
  // `WSADuplicateSocketW()` produces a `WSAPROTOCOL_INFOW` blob describing
  // the socket, which `WSASocketW(..., FROM_PROTOCOL_INFO, ...)` turns into
  // a new socket handle in the target process -- here, the same process,
  // since this is only used to hand a socket off from a temporary uv_tcp_t
  // to a fresh one on another loop.
  const SOCKET sock = reinterpret_cast<SOCKET>(fd);
  WSAPROTOCOL_INFOW info{};
  if (WSADuplicateSocketW(sock, GetCurrentProcessId(), &info) != 0)
    return INVALID_SOCKET;

  return WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0, 0);
#else
  if (uv_fileno(reinterpret_cast<uv_handle_t*>(handle), &fd) != 0)
    return -1;

  return dup(fd);
#endif
}

/**
 * @brief Tune a connected TCP handle for RPC-shaped traffic.
 *
 * Disables Nagle's algorithm (`TCP_NODELAY`) so small, frequent RMI
 * request/response frames aren't held back waiting to coalesce with more
 * outbound data, and enables TCP keepalive so a peer that vanishes without a
 * clean FIN/RST (e.g. a dropped network path) is eventually detected instead
 * of leaking that connection's session, threads, and buffers indefinitely.
 * Applied identically on the client and on every accepted server connection.
 * Failures are non-fatal -- worst case the connection keeps the OS default
 * behavior -- so the return codes are intentionally ignored.
 */
inline void tune_tcp_socket(uv_tcp_t* handle, unsigned int keepalive_delay_sec = 60) {
  uv_tcp_nodelay(handle, 1);
  uv_tcp_keepalive(handle, 1, keepalive_delay_sec);
}

} // namespace bdg::bison::rmi::transport
