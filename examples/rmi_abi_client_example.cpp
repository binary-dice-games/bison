// MIT License © 2025 Binary Dice Games
// examples/rmi_abi_client_example.cpp
//
// RMI client example using the Bison C ABI (rmi_c.h / bison_abi.dll).
//
// This file intentionally uses only the stable C ABI — no C++ templates or
// internal headers.  Include only "rmi_c.h".  Command-line flags mirror the
// --transport/--host/--port/--name names used by bison-cli
// (src/app/cli/main.cpp), so usage is consistent across the project.
// --transport=pty/console take no --host/--port/--name: like bison-cli, this
// process wraps its own already-connected fd 0/1 rather than spawning
// anything, so it works as the client half of `rmi_server_pty_create()` /
// `rmi_server_console_create()` (or the C++ `rmi_server_example --transport=
// pty|console`) without any extra setup.
//
// Run rmi_abi_server_example (tcp/pipe) or rmi_server_example (any
// transport) with matching flags before starting this client.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rmi_c.h"

static void print_usage(const char* program) {
  fprintf(
      stderr,
      "Usage: %s [--transport=tcp|pipe|pty|console] [--host=HOST] [--port=PORT] [--name=PATH]\n",
      program);
}

int main(int argc, char** argv) {
  const char* transport = "tcp";
  const char* host = "127.0.0.1";
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

  // ── Create and connect the client ────────────────────────────────────────
  rmi_client_handle client;
  if (strcmp(transport, "tcp") == 0) {
    client = rmi_client_tcp_create(host, port);
  } else if (strcmp(transport, "pipe") == 0) {
    client = rmi_client_pipe_create(name);
  } else if (strcmp(transport, "pty") == 0) {
    client = rmi_client_pty_create();
  } else if (strcmp(transport, "console") == 0) {
    client = rmi_client_console_create();
  } else {
    fprintf(stderr, "Invalid --transport: %s (expected tcp, pipe, pty, or console)\n", transport);
    return 1;
  }
  if (!client) {
    fprintf(stderr, "[Client] failed to allocate client\n");
    return 1;
  }

  rmi_error err = rmi_client_connect(client, NULL);
  if (err != RMI_OK) {
    fprintf(stderr, "[Client] connect failed (%d)\n", (int)err);
    rmi_client_release(client);
    return 1;
  }

  // ── Instantiate a Calculator on the server ────────────────────────────────
  rmi_proxy_handle calc = NULL;
  err = rmi_client_instantiate(client, 0, bison_key("Calculator"), NULL, &calc);
  if (err != RMI_OK || !calc) {
    fprintf(stderr, "[Client] instantiate failed (%d)\n", (int)err);
    rmi_client_disconnect(client);
    rmi_client_release(client);
    return 1;
  }
  printf("[Client] connected\n");

  // ── add(10, 3) ────────────────────────────────────────────────────────────
  {
    bison_handle params = bison_create(0);
    bison_set_float(params, bison_key("a"), 10.0f);
    bison_set_float(params, bison_key("b"), 3.0f);
    bison_handle result = NULL;
    rmi_proxy_call(calc, bison_key("add"), params, &result, -1);
    bison_release(params);
    if (result) {
      float res = 0.f;
      bison_get_float(result, bison_key("result"), &res);
      printf("[Client] add(10, 3) = %.0f\n", res);
      bison_release(result);
    }
  }

  // ── subtract(100, 21) ─────────────────────────────────────────────────────
  {
    bison_handle params = bison_create(0);
    bison_set_float(params, bison_key("a"), 100.0f);
    bison_set_float(params, bison_key("b"), 21.0f);
    bison_handle result = NULL;
    rmi_proxy_call(calc, bison_key("subtract"), params, &result, -1);
    bison_release(params);
    if (result) {
      float res = 0.f;
      bison_get_float(result, bison_key("result"), &res);
      printf("[Client] subtract(100, 21) = %.0f\n", res);
      bison_release(result);
    }
  }

  // ── multiply(7, 6) ────────────────────────────────────────────────────────
  {
    bison_handle params = bison_create(0);
    bison_set_float(params, bison_key("a"), 7.0f);
    bison_set_float(params, bison_key("b"), 6.0f);
    bison_handle result = NULL;
    rmi_proxy_call(calc, bison_key("multiply"), params, &result, -1);
    bison_release(params);
    if (result) {
      float res = 0.f;
      bison_get_float(result, bison_key("result"), &res);
      printf("[Client] multiply(7, 6) = %.0f\n", res);
      bison_release(result);
    }
  }

  // ── divide(42, 2) ────────────────────────────────────────────────────────
  {
    bison_handle params = bison_create(0);
    bison_set_float(params, bison_key("a"), 42.0f);
    bison_set_float(params, bison_key("b"), 2.0f);
    bison_handle result = NULL;
    rmi_proxy_call(calc, bison_key("divide"), params, &result, -1);
    bison_release(params);
    if (result) {
      float res = 0.f;
      bison_get_float(result, bison_key("result"), &res);
      printf("[Client] divide(42, 2) = %.0f\n", res);
      bison_release(result);
    }
  }

  // ── Clean up ─────────────────────────────────────────────────────────────
  // rmi_proxy_release sends a destroy request to the server before releasing.
  rmi_proxy_release(calc);
  rmi_client_disconnect(client);
  rmi_client_release(client);
  printf("[Client] done.\n");
  return 0;
}
