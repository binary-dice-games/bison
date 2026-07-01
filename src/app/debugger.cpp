#include <chrono>
#include <iostream>
#include <thread>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#include <fstream>
#include <string>
#endif

/**
 * @brief Blocks until a debugger is attached.
 */
void wait_for_debugger() {
  std::cout << "Waiting for debugger... (PID: " <<
#if defined(_WIN32) || defined(_WIN64)
      GetCurrentProcessId()
#else
      getpid()
#endif
            << ")" << std::endl;

  while (true) {
#if defined(_WIN32) || defined(_WIN64)
    if (IsDebuggerPresent()) {
      break;
    }
#elif defined(__linux__)
    std::ifstream statusFile("/proc/self/status");
    std::string line;
    bool debuggerPresent = false;

    while (std::getline(statusFile, line)) {
      if (line.compare(0, 10, "TracerPid:") == 0) {
        // Extraer el PID del proceso que está trazando/depurando a este
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
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::cout << "Debugger attached. Continuing execution..." << std::endl;
}
