// MIT License © 2025 Binary Dice Games
/**
 * @file socket_transport_platform.hpp
 * @brief Platform hook used by socket_transport.cpp to hand an accepted TCP
 *        connection off to a fresh uv_loop_t.
 *
 * libuv requires uv_accept()'s client handle to share the listening
 * handle's loop (`server->loop == client->loop`), but each accepted
 * connection here runs on its own dedicated uv_loop_t/thread. To bridge the
 * two loops, the connection is first accepted into a temporary handle on
 * the listener's loop, its OS socket is duplicated, and the duplicate is
 * attached (via uv_tcp_open) to a new handle on the connection's own loop.
 * Duplicating a socket descriptor has no libuv-portable API, so this one
 * step is implemented per-platform.
 *
 * Platform support:
 *  - Linux   — `dup()`.
 *  - Windows — `DuplicateHandle()` (Winsock sockets are kernel HANDLEs).
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
