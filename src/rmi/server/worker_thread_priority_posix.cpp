// MIT License © 2025 Binary Dice Games
/**
 * @file worker_thread_priority_posix.cpp
 * @brief Linux/MSYS2 implementation of worker_thread_priority.hpp.
 *
 * Uses `setpriority(PRIO_PROCESS, 0, ...)`: on Linux, each thread is its own
 * kernel task, and passing `who == 0` resolves to the calling task rather
 * than the thread-group leader, so this raises the *calling thread's* niceness
 * alone, not the whole process's.
 */
#include "src/rmi/server/worker_thread_priority.hpp"

#include <sys/resource.h>

namespace bdg::bison::rmi {

void lower_thread_priority_to_below_normal() {
  // +10 niceness is a modest, always-permitted (no CAP_SYS_NICE needed)
  // step below the default of 0 -- enough to yield to normal-priority
  // threads under contention without starving this thread entirely.
  setpriority(PRIO_PROCESS, 0, 10);
}

} // namespace bdg::bison::rmi
