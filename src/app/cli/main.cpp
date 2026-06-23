// MIT License © 2025 Binary Dice Games
/**
 * @file main.cpp
 * @brief Entry point for the standalone bison-cli interactive REPL.
 */
#include "src/app/cli/cli_app.hpp"

#include <gflags/gflags.h>

DEFINE_string(host,    "127.0.0.1", "Server host address");
DEFINE_int32 (port,    7070,        "Server TCP port");
DEFINE_string(pipe,    "",          "Named-pipe / Unix-socket path (empty = use socket)");
DEFINE_bool  (pty,     false,       "Use PTY transport (Linux only)");
DEFINE_int32 (timeout, 30000,       "Per-request timeout in milliseconds");

int main(int argc, char** argv) {
  bdg::bison::app::cli_app app;
  return app.run(argc, argv);
}
