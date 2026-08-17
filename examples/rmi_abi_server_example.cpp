// MIT License © 2025 Binary Dice Games
// examples/rmi_abi_server_example.cpp
//
// RMI server example using the Bison C ABI (rmi_c.h / bison_abi.dll).
//
// This file intentionally uses only the stable C ABI — no C++ templates or
// internal headers.  Include only "rmi_c.h".  Command-line flags (parsed
// with gflags) mirror the --transport/--host/--port/--name names used by
// calc-server (src/srv/calc/main.cpp), so usage is consistent across the
// project.  Only tcp and pipe are supported here; use rmi_server_example
// for the full set of transports.
//

#include <stdint.h>
#include <stdio.h>

// bison_abi.dll exports these flags (src/app/abi_flags.cpp), but MSVC never
// auto-imports DLL *data* symbols the way it does functions: without an
// explicit dllimport, the compiler emits a direct reference to the plain
// symbol name, which the linker can't resolve against the DLL's import
// library (LNK2001). Override gflags' own DLL_DECLARE_FLAG macro (used only
// by DECLARE_string/DECLARE_int32 below) so it expands with dllimport on
// native Windows; bundled gflags is built static here, so its generated
// default leaves this macro empty. Deliberately scoped to just the flag
// declarations, not GFLAGS_DLL_DECL itself -- that one also covers gflags'
// own API functions (ParseCommandLineFlags, SetUsageMessage, ...), which
// this example calls directly against the statically-linked gflags in its
// own binary, not against bison_abi.dll (nothing there references them, so
// they aren't part of its export table).
#if defined(_WIN32) && !defined(__CYGWIN__)
#define GFLAGS_DLL_DECLARE_FLAG __declspec(dllimport)
#endif
#include <gflags/gflags.h>

#include "rmi_c.h"

// bison_abi already defines --transport/--host/--port/--name (src/app/abi_flags.cpp,
// linked into every process that links bison_abi), so this example declares
// rather than redefines them, only overriding the default it wants below.
DECLARE_string(transport);
DECLARE_string(host);
DECLARE_int32(port);
DECLARE_string(name);

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
  gflags::SetCommandLineOptionWithMode("transport", "tcp", gflags::SET_FLAGS_DEFAULT);
  gflags::SetUsageMessage("RMI Calculator server example using the Bison C ABI.");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_port <= 0 || FLAGS_port > 65535) {
    fprintf(stderr, "Invalid --port: %d\n", FLAGS_port);
    return 1;
  }

  register_calculator();

  rmi_server_handle server;
  if (FLAGS_transport == "tcp") {
    server = rmi_server_tcp_create(FLAGS_host.c_str(), (uint16_t)FLAGS_port);
  } else if (FLAGS_transport == "pipe") {
    server = rmi_server_pipe_create(FLAGS_name.c_str());
  } else {
    fprintf(stderr, "transport '%s' is not supported by this example (supported: tcp, pipe)\n", FLAGS_transport.c_str());
    return 1;
  }
  if (!server) {
    fprintf(stderr, "[Server] failed to allocate server\n");
    return 1;
  }

  rmi_error err = rmi_server_listen(server, NULL, NULL, NULL);
  if (err != RMI_OK) {
    fprintf(stderr, "[Server] listen failed (%d)\n", (int)err);
    rmi_server_release(server);
    return 1;
  }

  if (FLAGS_transport == "pipe") {
    printf("[Server] Calculator listening on pipe %s\n", FLAGS_name.c_str());
  } else {
    printf("[Server] Calculator listening on %s:%u\n", FLAGS_host.c_str(), (unsigned)FLAGS_port);
  }
  printf("[Server] Press Enter to stop...\n");

  getchar();

  rmi_server_stop(server);
  rmi_server_release(server);
  printf("[Server] stopped.\n");
  return 0;
}
