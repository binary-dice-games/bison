// MIT License © 2025 Binary Dice Games
// examples/pty_abi_client_example.cpp
//
// PTY RMI client example using the Bison C ABI (pty_c.h / bison_abi).
//
// This file intentionally uses only the stable C ABI — no C++ headers.
// Run pty_abi_server_example first; SSH into the server host and launch this
// binary inside that SSH session so it uses the PTY channel as transport.
//
// Linux only.

#include <stdio.h>

#include "pty_c.h"

#ifdef __linux__

// ── Session callback ──────────────────────────────────────────────────────────

static int on_session(rmi_client_handle client, void* user) {
  (void)user;

  rmi_proxy_handle calc = NULL;
  rmi_error err =
      rmi_client_instantiate(client, 0, bison_key("Calculator"), NULL, &calc);
  if (err != RMI_OK || !calc) {
    fprintf(stderr, "[Client] instantiate failed (%d)\n", (int)err);
    return 1;
  }

  // ── add(10, 3) ──────────────────────────────────────────────────────────────
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
      fprintf(stderr, "[Client] add(10, 3) = %.0f\n", res);
      bison_release(result);
    }
  }

  // ── subtract(100, 21) ───────────────────────────────────────────────────────
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
      fprintf(stderr, "[Client] subtract(100, 21) = %.0f\n", res);
      bison_release(result);
    }
  }

  // ── multiply(7, 6) ──────────────────────────────────────────────────────────
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
      fprintf(stderr, "[Client] multiply(7, 6) = %.0f\n", res);
      bison_release(result);
    }
  }

  // ── divide(42, 2) ───────────────────────────────────────────────────────────
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
      fprintf(stderr, "[Client] divide(42, 2) = %.0f\n", res);
      bison_release(result);
    }
  }

  rmi_proxy_release(calc);
  fprintf(stderr, "[Client] done.\n");
  return 0;
}

static void on_connected(void* user) {
  (void)user;
  fprintf(stderr, "[Client] handshake complete — connected to Calculator server.\n");
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  pty_client_callbacks cb = {0};
  cb.on_session  = on_session;
  cb.on_connected = on_connected;
  return pty_client_run(argc, argv, &cb) == RMI_OK ? 0 : 1;
}

#else

int main(void) {
  fprintf(stderr, "pty_abi_client_example is only supported on Linux.\n");
  return 1;
}

#endif // __linux__
