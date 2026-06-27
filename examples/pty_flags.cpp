// MIT License © 2025 Binary Dice Games
/**
 * @file pty_flags.cpp
 * @brief Defines the gflags variables consumed by server_app / client_app
 *        for the pty_server_example and pty_client_example executables.
 */
#include <gflags/gflags.h>

DEFINE_string(host,    "0.0.0.0", "Bind/connect host address");
DEFINE_int32 (port,    7070,      "Listen/connect port");
DEFINE_string(pipe,    "",        "Named-pipe / Unix-socket path");
DEFINE_bool  (verbose, false,     "Print session trace messages to stdout");
DEFINE_bool  (pty,     true,      "Use PTY transport");
DEFINE_int32 (timeout, 30000,     "Connection timeout in milliseconds");
