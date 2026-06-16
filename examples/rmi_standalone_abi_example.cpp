// MIT License © 2025 Binary Dice Games
// examples/rmi_standalone_abi_example.cpp
//
// RMI standalone (in-process) example using the C ABI (bison_abi.dll) via the
// C++ RAII wrappers in include/rmi_c.hpp and include/bison_c.hpp.  Mirrors
// rmi_standalone_example.cpp but links only against bison_abi.
//
// Each thread creates its own standalone client via rmi_standalone_create().
// The rmi_c.hpp client wrapper does not expose a standalone factory, so a
// lightweight local RAII guard is used to manage the raw rmi_client_handle.
// The proxy RAII wrapper (proxy::own) is used to manage remote object lifetime.

#include "include/rmi_c.hpp"

#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace bdg::bison::abi;  // dynamic, ""_key
using namespace bdg::rmi::abi;    // proxy

// ─── Shared output mutex ──────────────────────────────────────────────────────
static std::mutex g_cout_mutex;

template <typename... Args>
static void println(Args&&... args) {
  std::lock_guard<std::mutex> lk(g_cout_mutex);
  (std::cout << ... << args) << '\n';
}

// ─── Local RAII guard for rmi_standalone_create() ────────────────────────────
// rmi_c.hpp's client wrapper only exposes a tcp() factory.  Standalone handles
// are managed here with a minimal guard that calls rmi_client_release() on
// destruction.
struct standalone_guard {
  rmi_client_handle handle;

  standalone_guard() : handle(rmi_standalone_create()) {
    if (!handle)
      throw std::runtime_error("rmi_standalone_create failed");
  }
  ~standalone_guard() {
    rmi_client_release(handle);
  }
  standalone_guard(const standalone_guard&) = delete;
  standalone_guard& operator=(const standalone_guard&) = delete;
};

// ─── Register Calculator class ────────────────────────────────────────────────
static void register_calculator() {
  auto proto = dynamic::create("Calculator"_key);

  proto.add_method(
      "add"_key,
      [](dynamic& /*self*/, const dynamic& params, dynamic& result) {
        float a = params.get<float>("a"_key);
        float b = params.get<float>("b"_key);
        result.set("result"_key, a + b);
      });

  proto.add_method(
      "subtract"_key,
      [](dynamic& /*self*/, const dynamic& params, dynamic& result) {
        float a = params.get<float>("a"_key);
        float b = params.get<float>("b"_key);
        result.set("result"_key, a - b);
      });

  proto.add_method(
      "multiply"_key,
      [](dynamic& /*self*/, const dynamic& params, dynamic& result) {
        float a = params.get<float>("a"_key);
        float b = params.get<float>("b"_key);
        result.set("result"_key, a * b);
      });

  proto.add_method(
      "divide"_key,
      [](dynamic& /*self*/, const dynamic& params, dynamic& result) {
        float a = params.get<float>("a"_key);
        float b = params.get<float>("b"_key);
        if (b == 0.0f) {
          result.set("error"_key, "division by zero");
          result.set("result"_key, 0.0f);
        } else {
          result.set("result"_key, a / b);
        }
      });

  dynamic::add_class(bison_hash{0}, proto);  // root class, global namespace
}

// ─── Client worker ────────────────────────────────────────────────────────────
static void run_client(int client_id) {
  // Each thread owns an independent in-process standalone client.
  standalone_guard guard;

  rmi_proxy_handle raw_proxy = nullptr;
  rmi_error err =
      rmi_client_instantiate(guard.handle, 0, bison_key("Calculator"), nullptr, &raw_proxy);
  if (err != RMI_OK || !raw_proxy)
    throw std::runtime_error("rmi_client_instantiate failed");

  proxy calc = proxy::own(raw_proxy);  // RAII ownership

  println("[Client ", client_id, "] connected");

  // add
  {
    auto params = dynamic::create();
    params.set("a"_key, float(10.0f * client_id));
    params.set("b"_key, 3.0f);
    auto result = calc.call("add"_key, params);
    println(
        "[Client ", client_id, "] add(", 10 * client_id, ", 3) = ",
        result.get<float>("result"_key));
  }

  // subtract
  {
    auto params = dynamic::create();
    params.set("a"_key, 100.0f);
    params.set("b"_key, float(7.0f * client_id));
    auto result = calc.call("subtract"_key, params);
    println(
        "[Client ", client_id, "] subtract(100, ", 7 * client_id, ") = ",
        result.get<float>("result"_key));
  }

  // multiply
  {
    auto params = dynamic::create();
    params.set("a"_key, float(client_id));
    params.set("b"_key, float(client_id));
    auto result = calc.call("multiply"_key, params);
    println(
        "[Client ", client_id, "] multiply(", client_id, ", ", client_id,
        ") = ", result.get<float>("result"_key));
  }

  // divide
  {
    auto params = dynamic::create();
    params.set("a"_key, 42.0f);
    params.set("b"_key, float(client_id));  // non-zero since client_id >= 1
    auto result = calc.call("divide"_key, params);
    println(
        "[Client ", client_id, "] divide(42, ", client_id, ") = ",
        result.get<float>("result"_key));
  }

  // proxy destructor sends destroy; guard destructor releases the client.
  println("[Client ", client_id, "] done.");
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main() {
  register_calculator();

  println("[Server] RMI Calculator standalone server started.");

  constexpr int NUM_CLIENTS = 3;
  std::vector<std::thread> threads;
  threads.reserve(NUM_CLIENTS);
  for (int i = 1; i <= NUM_CLIENTS; ++i) {
    threads.emplace_back(run_client, i);
  }

  for (auto& t : threads)
    t.join();

  println("[Server] all clients done.");

  return 0;
}
