// MIT License © 2025 Binary Dice Games
// examples/rmi_example.cpp
//
// Demonstrates the Bison RMI framework using the in-memory transport.
//
// Topology:
//   - One server hosting a "Calculator" class with add, subtract and multiply
//     methods.
//   - Three client threads that each instantiate a remote Calculator, perform
//     several operations concurrently, then clean up.
//
// Build: the CMakeLists in the examples folder adds this as the
//        "rmi_example" executable which links against the "bison" static lib.

#include "src/rmi/rmi.hpp"

#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

// ─── Namespaces
// ───────────────────────────────────────────────────────────────

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;
using namespace bdg::bison::rmi::shared::constants;

// ─── Shared output mutex (keeps std::cout lines from interleaving)
// ────────────

static std::mutex g_cout_mutex;

template <typename... Args>
static void println(Args&&... args) {
  std::lock_guard<std::mutex> lk(g_cout_mutex);
  (std::cout << ... << args) << '\n';
}

// ─── Register the Calculator class ───────────────────────────────────────────

static void register_calculator() {
  auto proto = dynamic_ptr{"Calculator"_key, {}};

  // add(a, b) → result
  proto->addMethod(
      "add"_key, [](dynamic& /*self*/, const dynamic& params) -> dynamic {
        float a = params["a"_key];
        float b = params["b"_key];
        dynamic result;
        result["result"_key] = a + b;
        return result;
      });

  // subtract(a, b) → result
  proto->addMethod(
      "subtract"_key, [](dynamic& /*self*/, const dynamic& params) -> dynamic {
        float a = params["a"_key];
        float b = params["b"_key];
        dynamic result;
        result["result"_key] = a - b;
        return result;
      });

  // multiply(a, b) → result
  proto->addMethod(
      "multiply"_key, [](dynamic& /*self*/, const dynamic& params) -> dynamic {
        float a = params["a"_key];
        float b = params["b"_key];
        dynamic result;
        result["result"_key] = a * b;
        return result;
      });

  // divide(a, b) → result  (returns 0 and logs error on division by zero)
  proto->addMethod(
      "divide"_key, [](dynamic& /*self*/, const dynamic& params) -> dynamic {
        float a = params["a"_key];
        float b = params["b"_key];
        dynamic result;
        if (b == 0.0f) {
          result["error"_key] = std::string{"division by zero"};
          result["result"_key] = 0.0f;
        } else {
          result["result"_key] = a / b;
        }
        return result;
      });

  dynamic::addClass(0U, proto);
}

// ─── Client worker
// ────────────────────────────────────────────────────────────

static void run_client(memory_server_transport& transport, int client_id) {
  // Each client gets its own isolated connection.
  client c{transport.connect()};
  c.connect();

  // Instantiate a Calculator on the server.
  auto calc = c.instantiate("Calculator"_key);

  println("[Client ", client_id, "] connected, object id=", calc.object_id());

  // ── add ──────────────────────────────────────────────────────────────────
  {
    dynamic params;
    params["a"_key] = float(10.0f * client_id);
    params["b"_key] = float(3.0f);
    auto result = calc.call("add"_key, std::move(params)).get();
    float res = result["result"_key];
    println("[Client ", client_id, "] add(", 10 * client_id, ", 3) = ", res);
  }

  // ── subtract ─────────────────────────────────────────────────────────────
  {
    dynamic params;
    params["a"_key] = float(100.0f);
    params["b"_key] = float(7.0f * client_id);
    auto result = calc.call("subtract"_key, std::move(params)).get();
    float res = result["result"_key];
    println(
        "[Client ", client_id, "] subtract(100, ", 7 * client_id, ") = ", res);
  }

  // ── multiply ─────────────────────────────────────────────────────────────
  {
    dynamic params;
    params["a"_key] = float(client_id);
    params["b"_key] = float(client_id);
    auto result = calc.call("multiply"_key, std::move(params)).get();
    float res = result["result"_key];
    println(
        "[Client ",
        client_id,
        "] multiply(",
        client_id,
        ", ",
        client_id,
        ") = ",
        res);
  }

  // ── divide ────────────────────────────────────────────────────────────────
  {
    dynamic params;
    params["a"_key] = float(42.0f);
    params["b"_key] = float(client_id); // non-zero since client_id >= 1
    auto result = calc.call("divide"_key, std::move(params)).get();
    float res = result["result"_key];
    println("[Client ", client_id, "] divide(42, ", client_id, ") = ", res);
  }

  c.destroy(std::move(calc));
  c.disconnect();
  println("[Client ", client_id, "] done.");
}

// ─── Main
// ─────────────────────────────────────────────────────────────────────

int main() {
  // Register the Calculator class in the global Bison class registry.
  register_calculator();

  // Create the in-memory transport — this is the connection hub.
  memory_server_transport transport;

  // Create and start the server.
  server srv{transport};
  srv.listen();
  println("[Server] RMI Calculator server started.");

  // Launch three client threads.
  constexpr int NUM_CLIENTS = 3;
  std::vector<std::thread> threads;
  threads.reserve(NUM_CLIENTS);
  for (int i = 1; i <= NUM_CLIENTS; ++i) {
    threads.emplace_back(run_client, std::ref(transport), i);
  }

  // Wait for all clients to finish.
  for (auto& t : threads)
    t.join();

  // Shut down the server cleanly.
  srv.stop();
  println("[Server] stopped.");

  return 0;
}
