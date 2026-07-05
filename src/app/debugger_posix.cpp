// MIT License © 2025 Binary Dice Games
/**
 * @file debugger_posix.cpp
 * @brief Linux/MSYS2 implementation of debugger.hpp, polling
 *        /proc/self/status for a non-zero TracerPid.
 */
#include "src/app/debugger.hpp"

#include <unistd.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

void wait_for_debugger() {
  std::cout << "Waiting for debugger... (PID: " << getpid() << ")" << std::endl;

  while (true) {
    std::ifstream statusFile("/proc/self/status");
    std::string line;
    bool debuggerPresent = false;

    while (std::getline(statusFile, line)) {
      if (line.compare(0, 10, "TracerPid:") == 0) {
        int tracerPid = std::stoi(line.substr(10));
        if (tracerPid > 0) {
          debuggerPresent = true;
        }
        break;
      }
    }

    if (debuggerPresent) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::cout << "Debugger attached. Continuing execution..." << std::endl;
}
