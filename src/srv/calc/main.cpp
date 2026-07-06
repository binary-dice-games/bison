// MIT License © 2025 Binary Dice Games
/**
 * @file main.cpp
 * @brief Entry point for the calc-server RMI server.
 */
#include "src/bison/bison_flags.hpp"
#include "src/srv/calc/calc_server.hpp"

#include <gflags/gflags.h>

DEFINE_string(transport, "term", "Transport to use: tcp, pipe or term");
DEFINE_string(host, "0.0.0.0", "Bind host address (transport=tcp)");
DEFINE_int32(port, 7070, "Listen port (transport=tcp)");
DEFINE_string(name, "", "Named-pipe / Unix-socket path (transport=pipe)");
DEFINE_string(cmd, "", "Command to spawn (transport=term)");
DEFINE_bool(verbose, false, "Print trace messages to stdout for debugging sessions");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");

int main(int argc, char** argv) {
  if (bdg::bison::print_usage(argc, argv, "Stateful arithmetic calculator server.", __FILE__))
    return 0;

  bdg::bison::srv::calc_server server;
  return server.run(argc, argv);
}
