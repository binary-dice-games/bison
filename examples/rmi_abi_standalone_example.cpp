// MIT License © 2025 Binary Dice Games
// examples/rmi_abi_standalone_example.cpp
//
// Standalone RMI example using the Bison C ABI (rmi_c.h / bison_abi.dll).
//
// This file intentionally uses only the stable C ABI for all Bison/RMI
// operations — no C++ templates or internal headers.  Include only "rmi_c.h".
// C++ threads are used purely for scheduling concurrent client workers.
//
// Topology:
//   - No separate server process.  rmi_standalone_create() returns an
//     in-process client that dispatches directly to the local class registry.
//   - Three client threads each instantiate a remote Calculator, perform
//     several operations concurrently, then clean up.

#include <stdint.h>
#include <stdio.h>

#include <mutex>
#include <thread>
#include <vector>

#include "rmi_c.h"

// ── Shared output mutex ───────────────────────────────────────────────────────

static std::mutex g_print_mutex;

// ── Method implementations ────────────────────────────────────────────────────

static void method_add(
    bison_handle self, bison_handle params, bison_handle result, void* user) {
  (void)self;
  (void)user;
  float a = 0.f, b = 0.f;
  bison_get_float(params, bison_key("a"), &a);
  bison_get_float(params, bison_key("b"), &b);
  bison_set_float(result, bison_key("result"), a + b);
}

static void method_subtract(
    bison_handle self, bison_handle params, bison_handle result, void* user) {
  (void)self;
  (void)user;
  float a = 0.f, b = 0.f;
  bison_get_float(params, bison_key("a"), &a);
  bison_get_float(params, bison_key("b"), &b);
  bison_set_float(result, bison_key("result"), a - b);
}

static void method_multiply(
    bison_handle self, bison_handle params, bison_handle result, void* user) {
  (void)self;
  (void)user;
  float a = 0.f, b = 0.f;
  bison_get_float(params, bison_key("a"), &a);
  bison_get_float(params, bison_key("b"), &b);
  bison_set_float(result, bison_key("result"), a * b);
}

static void method_divide(
    bison_handle self, bison_handle params, bison_handle result, void* user) {
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
  bison_add_method(proto, bison_key("add"),      method_add,      NULL, NULL);
  bison_add_method(proto, bison_key("subtract"), method_subtract, NULL, NULL);
  bison_add_method(proto, bison_key("multiply"), method_multiply, NULL, NULL);
  bison_add_method(proto, bison_key("divide"),   method_divide,   NULL, NULL);
  bison_add_class(0, proto, 0, NULL);
  bison_release(proto);
}

// ── Client worker ─────────────────────────────────────────────────────────────

static void run_client(int client_id) {
  // Each client gets its own standalone in-process connection.
  rmi_client_handle client = rmi_standalone_create();
  rmi_client_connect(client, NULL); // no-op for standalone

  // Instantiate a Calculator in-process.
  rmi_proxy_handle calc = NULL;
  rmi_client_instantiate(client, 0, bison_key("Calculator"), NULL, &calc);

  {
    std::lock_guard<std::mutex> lk(g_print_mutex);
    printf("[Client %d] connected\n", client_id);
  }

  // ── add ──────────────────────────────────────────────────────────────────
  {
    float a = 10.0f * client_id;
    bison_handle params = bison_create(0);
    bison_set_float(params, bison_key("a"), a);
    bison_set_float(params, bison_key("b"), 3.0f);
    bison_handle result = NULL;
    rmi_proxy_call(calc, bison_key("add"), params, &result, -1);
    bison_release(params);
    if (result) {
      float res = 0.f;
      bison_get_float(result, bison_key("result"), &res);
      std::lock_guard<std::mutex> lk(g_print_mutex);
      printf("[Client %d] add(%.0f, 3) = %.0f\n", client_id, a, res);
      bison_release(result);
    }
  }

  // ── subtract ─────────────────────────────────────────────────────────────
  {
    float b = 7.0f * client_id;
    bison_handle params = bison_create(0);
    bison_set_float(params, bison_key("a"), 100.0f);
    bison_set_float(params, bison_key("b"), b);
    bison_handle result = NULL;
    rmi_proxy_call(calc, bison_key("subtract"), params, &result, -1);
    bison_release(params);
    if (result) {
      float res = 0.f;
      bison_get_float(result, bison_key("result"), &res);
      std::lock_guard<std::mutex> lk(g_print_mutex);
      printf("[Client %d] subtract(100, %.0f) = %.0f\n", client_id, b, res);
      bison_release(result);
    }
  }

  // ── multiply ─────────────────────────────────────────────────────────────
  {
    float v = (float)client_id;
    bison_handle params = bison_create(0);
    bison_set_float(params, bison_key("a"), v);
    bison_set_float(params, bison_key("b"), v);
    bison_handle result = NULL;
    rmi_proxy_call(calc, bison_key("multiply"), params, &result, -1);
    bison_release(params);
    if (result) {
      float res = 0.f;
      bison_get_float(result, bison_key("result"), &res);
      std::lock_guard<std::mutex> lk(g_print_mutex);
      printf(
          "[Client %d] multiply(%.0f, %.0f) = %.0f\n",
          client_id, v, v, res);
      bison_release(result);
    }
  }

  // ── divide ────────────────────────────────────────────────────────────────
  {
    float b = (float)client_id; // non-zero since client_id >= 1
    bison_handle params = bison_create(0);
    bison_set_float(params, bison_key("a"), 42.0f);
    bison_set_float(params, bison_key("b"), b);
    bison_handle result = NULL;
    rmi_proxy_call(calc, bison_key("divide"), params, &result, -1);
    bison_release(params);
    if (result) {
      float res = 0.f;
      bison_get_float(result, bison_key("result"), &res);
      std::lock_guard<std::mutex> lk(g_print_mutex);
      printf("[Client %d] divide(42, %.0f) = %.0f\n", client_id, b, res);
      bison_release(result);
    }
  }

  rmi_proxy_release(calc);
  rmi_client_disconnect(client); // no-op for standalone
  rmi_client_release(client);

  std::lock_guard<std::mutex> lk(g_print_mutex);
  printf("[Client %d] done.\n", client_id);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(void) {
  // Register the Calculator class in the global Bison class registry.
  register_calculator();

  printf("[Server] RMI Calculator registered (standalone in-process mode).\n");

  // Launch three client threads.
  constexpr int NUM_CLIENTS = 3;
  std::vector<std::thread> threads;
  threads.reserve(NUM_CLIENTS);
  for (int i = 1; i <= NUM_CLIENTS; ++i)
    threads.emplace_back(run_client, i);

  for (auto& t : threads)
    t.join();

  printf("[Server] all clients done.\n");
  return 0;
}
