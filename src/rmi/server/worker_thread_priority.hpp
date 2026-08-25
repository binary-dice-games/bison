// MIT License © 2025 Binary Dice Games
/**
 * @file worker_thread_priority.hpp
 * @brief Lower the calling thread's OS scheduling priority, for background
 *        worker threads that must never compete with the application's own
 *        threads for CPU time. Implemented per-platform
 *        (`worker_thread_priority_posix.cpp`/`worker_thread_priority_win.cpp`)
 *        since adjusting a single thread's priority uses OS-specific
 *        facilities.
 */
#pragma once

namespace bdg::bison::rmi {

/**
 * @brief Lower the calling thread's scheduling priority to "below normal",
 *        best-effort.
 *
 * Must be called from the thread whose priority is being lowered. Failure
 * (e.g. insufficient privilege) is silently ignored -- this is a throughput
 * hint, not a correctness requirement.
 */
void lower_thread_priority_to_below_normal();

} // namespace bdg::bison::rmi
