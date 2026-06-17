// MIT License © 2025 Binary Dice Games
// examples/rmi_abi_client_example.cpp
//
// RMI client example using the Bison C ABI (rmi_c.h / bison_abi.dll).
//
// This file intentionally uses only the stable C ABI — no C++ templates or
// internal headers.  Include only "rmi_c.h".
//
// Run rmi_abi_server_example on the same host and port before starting this
// client.  Optional command-line arguments: <host> <port> (defaults: 127.0.0.1
// 7070).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "rmi_c.h"

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

  // ── Create and connect the TCP client ────────────────────────────────────
  rmi_client_handle client = rmi_client_tcp_create(host, port);
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
  err = rmi_client_instantiate(
      client, 0, bison_key("Calculator"), NULL, &calc);
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
