// MIT License © 2025 Binary Dice Games
/**
 * @file abi_flags.cpp
 * @brief Defines the gflags variables required by server_app / client_app
 *        when those modules are pulled into bison_abi.dll.
 *
 * In executable builds these flags are defined by the binary's own main.cpp.
 * bison_abi.dll has no main.cpp, so they are defined here instead.
 * This file must NOT be compiled into the bison static library, only into
 * bison_abi, to avoid duplicate-symbol conflicts with host executables.
 */
#include <gflags/gflags.h>

// ── Transport flags — consumed by server_app and client_app ──────────────────
DEFINE_string(host, "0.0.0.0", "Bind/connect host address");
DEFINE_int32(port, 7070, "Listen/connect port");
DEFINE_string(pipe, "", "Named-pipe / Unix-socket path");
DEFINE_bool(verbose, false, "Print session trace messages to stdout");
DEFINE_int32(timeout, 30000, "Connection timeout in milliseconds");
DEFINE_string(pty, "", "Command to run inside a PTY (enables PTY transport; e.g. /bin/bash)");
DEFINE_int32(pty_cols, 220, "Terminal column width for PTY transport");
DEFINE_int32(pty_rows, 50, "Terminal row height for PTY transport");
