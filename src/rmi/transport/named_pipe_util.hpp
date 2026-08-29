// MIT License © 2025 Binary Dice Games
/**
 * @file named_pipe_util.hpp
 * @brief Shared named-pipe / Unix-socket handle helpers for libuv-backed
 *        transports.
 */
#pragma once

#include <uv.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace bdg::bison::rmi::transport {

/**
 * @brief Duplicate the OS handle underlying an open `uv_pipe_t` handle.
 *
 * A plain `dup()` works for a POSIX Unix-domain-socket fd but not for a
 * Windows named-pipe `HANDLE`, which requires `DuplicateHandle()` plus
 * `_open_osfhandle()` to turn the duplicated `HANDLE` back into the CRT file
 * descriptor `uv_pipe_open()` expects. Used by the accept path of
 * `named_pipe_server_transport` to hand an accepted connection off from the
 * listener's `uv_loop_t` to a fresh `uv_pipe_t` on the connection's own
 * dedicated loop -- `uv_accept()` requires the target handle to already
 * share the listener's loop, so the connection's own loop can't be used
 * directly (same hand-off this transport's TCP counterpart performs; see
 * `tcp_socket_util.hpp`'s `duplicate_tcp_socket()` and
 * `src/rmi/DESIGN.md` §2).
 *
 * @return The duplicated handle as a `uv_file` usable with `uv_pipe_open()`,
 *         or a negative value on failure.
 */
inline uv_file duplicate_pipe_handle(uv_pipe_t* handle) {
  uv_os_fd_t fd{};
  if (uv_fileno(reinterpret_cast<uv_handle_t*>(handle), &fd) != 0)
    return -1;

#if defined(_WIN32) || defined(__CYGWIN__)
  // `fd` is already the underlying Win32 HANDLE for a uv_pipe_t. Duplicate
  // it within this process, then wrap the duplicate as a CRT file
  // descriptor -- the form uv_pipe_open() expects -- via
  // _open_osfhandle(); libuv converts it back to a HANDLE internally.
  HANDLE dup_handle = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), fd, GetCurrentProcess(), &dup_handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
    return -1;

  const intptr_t crt_fd = _open_osfhandle(reinterpret_cast<intptr_t>(dup_handle), 0);
  if (crt_fd == -1) {
    CloseHandle(dup_handle);
    return -1;
  }
  return static_cast<uv_file>(crt_fd);
#else
  return dup(fd);
#endif
}

} // namespace bdg::bison::rmi::transport
