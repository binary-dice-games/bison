// MIT License © 2025 Binary Dice Games
// examples/rmi_abi_server_example.cpp
//
// RMI server example using the Bison C ABI (rmi_c.h / bison_abi.dll).
//
// This file intentionally uses only the stable C ABI — no C++ templates or
// internal headers.  Include only "rmi_c.h".
//
// Registers a Calculator class with add, subtract, multiply, and divide
// methods, then listens for client connections on host:port.  Press Enter to
// stop.  Optional command-line arguments: <host> <port> (defaults: 127.0.0.1
// 7070).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "rmi_c.h"

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
  const char* host = "127.0.0.1";
  uint16_t port = 7070;

  if (argc > 1)
    host = argv[1];
  if (argc > 2) {
    unsigned long parsed = strtoul(argv[2], NULL, 10);
    if (parsed > 0 && parsed <= 65535)
      port = (uint16_t)parsed;
  }

  register_calculator();

  rmi_server_handle server = rmi_server_tcp_create(host, port);
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

  printf("[Server] Calculator listening on %s:%u\n", host, (unsigned)port);
  printf("[Server] Press Enter to stop...\n");

  getchar();

  rmi_server_stop(server);
  rmi_server_release(server);
  printf("[Server] stopped.\n");
  return 0;
}
