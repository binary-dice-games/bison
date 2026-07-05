// MIT License © 2025 Binary Dice Games
/**
 * @file fd_dup.hpp
 * @brief Duplicates a plain file descriptor. Implemented per-platform
 *        (`fd_dup_posix.cpp`/`fd_dup_win.cpp`) since the underlying OS call
 *        differs (`dup()` vs. the CRT's `_dup()`), even though both take and
 *        return a plain `int`.
 */
#pragma once

namespace bdg::bison::rmi::transport {

/**
 * @brief Duplicates @p fd.
 * @return A new descriptor referring to the same underlying file/pipe, or a
 *         negative value on failure.
 */
int dup_fd(int fd);

} // namespace bdg::bison::rmi::transport
