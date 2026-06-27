// MIT License © 2025 Binary Dice Games
// examples/pty_abi_server_example.cpp
//
// PTY RMI server example using the Bison C ABI (pty_c.h / bison_abi).
//
// This file intentionally uses only the stable C ABI — no C++ headers.
// Registers a Calculator class, then spawns a child process via uv_spawn()
// and communicates with it over stdin/stdout pipes using bison 4-byte-length-
// prefix framing.
//
// The child process set via shell_command callback can be:
//   Local:  "./pty_abi_client_example"
//   Remote: "ssh user@remote-host ./pty_abi_client_example"
//
// SSH passes stdin/stdout through as binary pipes, so bison frames are
// relayed transparently to the client running on the remote machine.
//
// Cross-platform (Windows and Linux).

#include <stdio.h>
#include <string.h>

#include "pty_c.h"

static const char* g_spawn_cmd = NULL;

// ── Method implementations ────────────────────────────────────────────────────

static void method_add(
    bison_handle self, bison_handle params, bison_handle result, void* user) {
  (void)self; (void)user;
  float a = 0.f, b = 0.f;
  bison_get_float(params, bison_key("a"), &a);
  bison_get_float(params, bison_key("b"), &b);
  bison_set_float(result, bison_key("result"), a + b);
}

static void method_subtract(
    bison_handle self, bison_handle params, bison_handle result, void* user) {
  (void)self; (void)user;
  float a = 0.f, b = 0.f;
  bison_get_float(params, bison_key("a"), &a);
  bison_get_float(params, bison_key("b"), &b);
  bison_set_float(result, bison_key("result"), a - b);
}

static void method_multiply(
    bison_handle self, bison_handle params, bison_handle result, void* user) {
  (void)self; (void)user;
  float a = 0.f, b = 0.f;
  bison_get_float(params, bison_key("a"), &a);
  bison_get_float(params, bison_key("b"), &b);
  bison_set_float(result, bison_key("result"), a * b);
}

static void method_divide(
    bison_handle self, bison_handle params, bison_handle result, void* user) {
  (void)self; (void)user;
  float a = 0.f, b = 0.f;
  bison_get_float(params, bison_key("a"), &a);
  bison_get_float(params, bison_key("b"), &b);
  if (b == 0.0f) {
    bison_set_string(result, bison_key("error"), "division by zero");
    bison_set_float(result, bison_key("result"), 0.0f);
  } else {
    bison_set_float(result, bison_key("result"), a / b);
  }
}

// ── Callbacks ─────────────────────────────────────────────────────────────────

static void register_classes(void* user) {
  (void)user;
  bison_handle proto = bison_create(bison_key("Calculator"));
  bison_add_method(proto, bison_key("add"),      method_add,      NULL, NULL);
  bison_add_method(proto, bison_key("subtract"), method_subtract, NULL, NULL);
  bison_add_method(proto, bison_key("multiply"), method_multiply, NULL, NULL);
  bison_add_method(proto, bison_key("divide"),   method_divide,   NULL, NULL);
  bison_add_class(0, proto, 0, NULL);
  bison_release(proto);
}

static void on_client_connected(void* user) {
  (void)user;
  fprintf(stderr, "[Server] bison client connected — serving Calculator.\n");
}

static void on_session_ended(void* user) {
  (void)user;
  fprintf(stderr, "[Server] session ended.\n");
}

static const char* get_shell_command(void* user) {
  (void)user;
  if (g_spawn_cmd && g_spawn_cmd[0]) return g_spawn_cmd;
#if defined(_WIN32)
  return "pty_abi_client_example.exe";
#else
  return "./pty_abi_client_example";
#endif
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  if (argc > 1) g_spawn_cmd = argv[1];
  fprintf(stderr, "[Server] starting PTY server — spawning client subprocess.\n");

  pty_server_callbacks cb = {0};
  cb.register_classes    = register_classes;
  cb.shell_command       = get_shell_command;
  cb.on_client_connected = on_client_connected;
  cb.on_session_ended    = on_session_ended;
  return pty_server_run(argc, argv, &cb) == RMI_OK ? 0 : 1;
}
