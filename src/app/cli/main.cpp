// MIT License © 2025 Binary Dice Games
/**
 * @file main.cpp
 * @brief Entry point for the standalone bison-cli interactive REPL.
 */
#include "src/app/cli/cli_app.hpp"
#include "src/bison/bison_flags.hpp"

#include <gflags/gflags.h>

DEFINE_string(transport, "tcp", "Transport to use: tcp, pipe, or pty");
DEFINE_string(host, "127.0.0.1", "Server host address (transport=tcp)");
DEFINE_int32(port, 7070, "Server TCP port (transport=tcp)");
DEFINE_string(name, "", "Named-pipe / Unix-socket path (transport=pipe)");
DEFINE_int32(timeout, 30000, "Per-request timeout in milliseconds");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");

int main(int argc, char** argv) {
  if (bdg::bison::print_usage(argc, argv, "Interactive REPL client for bison RMI servers.", __FILE__))
    return 0;

  bdg::bison::app::cli_app app;
  return app.run(argc, argv);
}
