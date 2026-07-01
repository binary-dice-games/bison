// MIT License © 2025 Binary Dice Games
/**
 * @file main.cpp
 * @brief Entry point for the calc-server RMI server.
 */
#include "src/srv/calc/calc_server.hpp"

#include <gflags/gflags.h>

DEFINE_string(host, "0.0.0.0", "Bind host address");
DEFINE_int32(port, 7070, "Listen port");
DEFINE_string(pipe, "", "Named-pipe / Unix-socket path (empty = use socket)");
DEFINE_bool(verbose, false, "Print trace messages to stdout for debugging sessions");
DEFINE_string(pty, "", "Command to run inside a PTY (enables PTY transport; e.g. /bin/bash)");
DEFINE_int32(pty_cols, 220, "Terminal column width for PTY transport");
DEFINE_int32(pty_rows, 50, "Terminal row height for PTY transport");

int main(int argc, char** argv) {
  bdg::bison::srv::calc_server server;
  return server.run(argc, argv);
}
