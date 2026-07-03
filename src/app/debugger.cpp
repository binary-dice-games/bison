#include <chrono>
#include <iostream>
#include <thread>

#include <unistd.h>
#include <fstream>
#include <string>

/**
 * @brief Blocks until a debugger is attached.
 */
void wait_for_debugger() {
  std::cout << "Waiting for debugger... (PID: " << getpid() << ")" << std::endl;

  while (true) {
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
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::cout << "Debugger attached. Continuing execution..." << std::endl;
}
