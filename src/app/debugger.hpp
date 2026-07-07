// MIT License © 2025 Binary Dice Games
/**
 * @file debugger.hpp
 * @brief Debugger-attach helper for the `--debugger` flag. Implemented
 *        per-platform (`debugger_posix.cpp`/`debugger_win.cpp`) since
 *        detecting an attached debugger uses OS-specific facilities.
 */
#pragma once

/**
 * @brief Blocks until a debugger is attached.
 */
void wait_for_debugger();
