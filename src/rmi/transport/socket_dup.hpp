// MIT License © 2025 Binary Dice Games
/**
 * @file socket_dup.hpp
 * @brief Duplicates the OS socket underlying an open uv_tcp_t handle.
 *        Implemented per-platform (`socket_dup_posix.cpp`/`socket_dup_win.cpp`)
 *        since a plain `dup()` works for a POSIX socket fd but not for a
 *        Winsock `SOCKET`, which requires `WSADuplicateSocket`.
 */
#pragma once

#include <uv.h>

namespace bdg::bison::rmi::transport {

/**
 * @brief Duplicate the OS socket underlying @p handle.
 * @param handle An open, connected uv_tcp_t (e.g. one just populated by uv_accept).
 * @return A duplicate socket descriptor suitable for uv_tcp_open() on another
 *         loop, or an invalid descriptor on failure.
 */
uv_os_sock_t duplicate_tcp_socket(uv_tcp_t* handle);

} // namespace bdg::bison::rmi::transport
