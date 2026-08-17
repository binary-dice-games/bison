// MIT License © 2025 Binary Dice Games
// examples/rmi_abi_client_example.cpp
//
// RMI client example using the Bison C ABI (rmi_c.h / bison_abi.dll).
//
// This file intentionally uses only the stable C ABI — no C++ templates or
// internal headers.  Include only "rmi_c.h".  Command-line flags (parsed
// with gflags) mirror the --transport/--host/--port/--name names used by
// bison-cli (src/app/cli/main.cpp), so usage is consistent across the
// project.  Only tcp and pipe are supported here; use rmi_client_example
// for the full set of transports (including term).
//
// Run rmi_abi_server_example (tcp/pipe) or rmi_server_example (any
// transport) with matching flags before starting this client.

#include <stdint.h>
#include <stdio.h>

#include <gflags/gflags.h>

#include "rmi_c.h"

// bison_abi already defines --transport/--host/--port/--name (src/app/abi_flags.cpp,
// linked into every process that links bison_abi), so this example declares
// rather than redefines them, only overriding the defaults it wants below.
DECLARE_string(transport);
DECLARE_string(host);
DECLARE_int32(port);
DECLARE_string(name);

int main(int argc, char** argv) {
  gflags::SetCommandLineOptionWithMode("transport", "tcp", gflags::SET_FLAGS_DEFAULT);
  gflags::SetCommandLineOptionWithMode("host", "127.0.0.1", gflags::SET_FLAGS_DEFAULT);
  gflags::SetUsageMessage("RMI Calculator client example using the Bison C ABI.");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_port <= 0 || FLAGS_port > 65535) {
    fprintf(stderr, "Invalid --port: %d\n", FLAGS_port);
    return 1;
  }

  // ── Create and connect the client ────────────────────────────────────────
  rmi_client_handle client;
  if (FLAGS_transport == "tcp") {
    client = rmi_client_tcp_create(FLAGS_host.c_str(), (uint16_t)FLAGS_port);
  } else if (FLAGS_transport == "pipe") {
    client = rmi_client_pipe_create(FLAGS_name.c_str());
  } else {
    fprintf(stderr, "Invalid --transport: %s (expected tcp, pipe)\n", FLAGS_transport.c_str());
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
