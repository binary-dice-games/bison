// MIT License © 2025 Binary Dice Games
// examples/rmi_abi_server_example.cpp
//
// RMI server example using the Bison C ABI (rmi_c.h / bison_abi.dll).
//
// This file intentionally uses only the stable C ABI — no C++ templates or
// internal headers.  Include only "rmi_c.h".  Command-line flags mirror the
// --transport/--host/--port/--name names used by calc-server
// (src/srv/calc/main.cpp), so usage is consistent across the project; the C
// ABI only exposes tcp and pipe transports (no pty/console).
//
// Registers a Calculator class with add, subtract, multiply, and divide
// methods, then listens for client connections.  Press Enter to stop.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rmi_c.h"

static void print_usage(const char* program) {
  fprintf(
      stderr,
      "Usage: %s [--transport=tcp|pipe] [--host=HOST] [--port=PORT] [--name=PATH]\n",
      program);
}

// ── Method implementations ────────────────────────────────────────────────────

static void method_add(bison_handle self, bison_handle params, bison_handle result, void* user) {
  (void)self;
  (void)user;
  float a = 0.f, b = 0.f;
  bison_get_float(params, bison_key("a"), &a);
  bison_get_float(params, bison_key("b"), &b);
  bison_set_float(result, bison_key("result"), a + b);
}

static void method_subtract(bison_handle self, bison_handle params, bison_handle result, void* user) {
  (void)self;
  (void)user;
  float a = 0.f, b = 0.f;
  bison_get_float(params, bison_key("a"), &a);
  bison_get_float(params, bison_key("b"), &b);
  bison_set_float(result, bison_key("result"), a - b);
}

static void method_multiply(bison_handle self, bison_handle params, bison_handle result, void* user) {
  (void)self;
  (void)user;
  float a = 0.f, b = 0.f;
  bison_get_float(params, bison_key("a"), &a);
  bison_get_float(params, bison_key("b"), &b);
  bison_set_float(result, bison_key("result"), a * b);
}

static void method_divide(bison_handle self, bison_handle params, bison_handle result, void* user) {
  (void)self;
  (void)user;
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

// ── Class registration ────────────────────────────────────────────────────────

static void register_calculator(void) {
  bison_handle proto = bison_create(bison_key("Calculator"));
  bison_add_method(proto, bison_key("add"), method_add, NULL, NULL);
  bison_add_method(proto, bison_key("subtract"), method_subtract, NULL, NULL);
  bison_add_method(proto, bison_key("multiply"), method_multiply, NULL, NULL);
  bison_add_method(proto, bison_key("divide"), method_divide, NULL, NULL);
  bison_add_class(0, proto, 0, NULL);
  bison_release(proto);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  const char* transport = "tcp";
  const char* host = "0.0.0.0";
  uint16_t port = 7070;
  const char* name = "";

  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      print_usage(argv[0]);
      return 0;
    } else if (strncmp(arg, "--transport=", 12) == 0) {
      transport = arg + 12;
    } else if (strncmp(arg, "--host=", 7) == 0) {
      host = arg + 7;
    } else if (strncmp(arg, "--port=", 7) == 0) {
      unsigned long parsed = strtoul(arg + 7, NULL, 10);
      if (parsed == 0 || parsed > 65535) {
        fprintf(stderr, "Invalid --port: %s\n", arg + 7);
        return 1;
      }
      port = (uint16_t)parsed;
    } else if (strncmp(arg, "--name=", 7) == 0) {
      name = arg + 7;
    } else {
      fprintf(stderr, "Unknown option: %s\n", arg);
      print_usage(argv[0]);
      return 1;
    }
  }

  register_calculator();

  rmi_server_handle server;
  if (strcmp(transport, "tcp") == 0) {
    server = rmi_server_tcp_create(host, port);
  } else if (strcmp(transport, "pipe") == 0) {
    server = rmi_server_pipe_create(name);
  } else {
    fprintf(stderr, "transport '%s' is not supported by the C ABI example (supported: tcp, pipe)\n", transport);
    return 1;
  }
  if (!server) {
    fprintf(stderr, "[Server] failed to allocate server\n");
    return 1;
  }

  rmi_error err = rmi_server_listen(server, NULL);
  if (err != RMI_OK) {
    fprintf(stderr, "[Server] listen failed (%d)\n", (int)err);
    rmi_server_release(server);
    return 1;
  }

  if (strcmp(transport, "pipe") == 0) {
    printf("[Server] Calculator listening on pipe %s\n", name);
  } else {
    printf("[Server] Calculator listening on %s:%u\n", host, (unsigned)port);
  }
  printf("[Server] Press Enter to stop...\n");

  getchar();

  rmi_server_stop(server);
  rmi_server_release(server);
  printf("[Server] stopped.\n");
  return 0;
}
