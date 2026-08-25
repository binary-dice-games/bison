// MIT License © 2025 Binary Dice Games
/**
 * @file worker_thread_priority_win.cpp
 * @brief Native Windows implementation of worker_thread_priority.hpp, using
 *        `SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL)`.
 */
#include "src/rmi/server/worker_thread_priority.hpp"

#include <windows.h>

namespace bdg::bison::rmi {

void lower_thread_priority_to_below_normal() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}

} // namespace bdg::bison::rmi
