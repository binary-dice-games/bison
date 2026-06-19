// MIT License © 2025 Binary Dice Games
/**
 * @file main.cpp
 * @brief Entry point for the calc-server RMI server.
 */
#include "src/srv/calc/calc_server.hpp"

#include <gflags/gflags.h>

DEFINE_string(host, "0.0.0.0", "Bind host address");
DEFINE_int32 (port, 7070,      "Listen port");

int main(int argc, char** argv) {
  bdg::bison::srv::calc_server server;
  return server.run(argc, argv);
}
