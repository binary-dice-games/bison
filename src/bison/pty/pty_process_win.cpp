// MIT License © 2025 Binary Dice Games
/**
 * @file pty_process_win.cpp
 * @brief Windows stub for pty_process — not implemented.
 */
#include "src/bison/pty/pty_process.hpp"

#include <stdexcept>

namespace bdg::bison::pty {

/** @brief Unused on Windows; the constructor always throws first. */
struct pty_process_state {};

pty_process::pty_process(const std::string& /*cmd*/) {
  throw std::runtime_error("pty_process: not implemented on Windows");
}

pty_process::~pty_process() = default;

int pty_process::master_fd() const {
  return master_fd_;
}

void pty_process::start_pump() {}

int pty_process::wait() {
  return -1;
}

void pty_process::pump_loop() {}

} // namespace bdg::bison::pty
