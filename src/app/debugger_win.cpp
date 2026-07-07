// MIT License © 2025 Binary Dice Games
/**
 * @file debugger_win.cpp
 * @brief Native Windows implementation of debugger.hpp, polling
 *        IsDebuggerPresent().
 */
#include "src/app/debugger.hpp"

#include <windows.h>

#include <chrono>
#include <iostream>
#include <thread>

void wait_for_debugger() {
  std::cout << "Waiting for debugger... (PID: " << GetCurrentProcessId() << ")" << std::endl;

  while (!IsDebuggerPresent()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::cout << "Debugger attached. Continuing execution..." << std::endl;
}
