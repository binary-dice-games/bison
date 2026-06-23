// MIT License © 2025 Binary Dice Games
/**
 * @file app_test_flags.cpp
 * @brief gflags definitions required by srv_app / client_app when linked into
 *        test executables.
 *
 * srv_app and client_app use DECLARE_* for flags that are normally DEFINE_* in
 * a server or client binary's main.cpp.  Any test executable that pulls in
 * those scaffolds (transitively via pty_server_app, pty_client_app, etc.) must
 * provide definitions so the linker can resolve the references.
 */
#include <gflags/gflags.h>

DEFINE_string(host,    "127.0.0.1", "Server host address (test default)");
DEFINE_int32 (port,    7070,        "Server port (test default)");
DEFINE_string(pipe,    "",          "Named-pipe path (test default)");
DEFINE_bool  (pty,     false,       "Use PTY transport (test default)");
DEFINE_bool  (verbose, false,       "Verbose trace (test default)");
DEFINE_int32 (timeout, 30000,       "Per-request timeout ms (test default)");
