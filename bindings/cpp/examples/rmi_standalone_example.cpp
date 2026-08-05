// MIT License © 2025 Binary Dice Games
// bindings/cpp/examples/rmi_standalone_example.cpp
//
// Standalone (in-process, no network) RMI example using the header-only
// C++ ABI binding (bindings/cpp/include/bison/rmi.hpp). Mirrors
// examples/rmi_abi_standalone_example.cpp: three worker threads each
// instantiate their own in-process Calculator and perform several
// operations concurrently.
//
// Run: ./rmi_cpp_standalone_example

#include "bison/rmi.hpp"

#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

using namespace bdg::bison::abi;

namespace {

std::mutex g_print_mutex;

void register_calculator() {
  dynamic proto{"Calculator"_key};
  proto.addMethod("add"_key, [](dynamic&, const dynamic& params, dynamic& result) {
    result["result"_key] = params["a"_key].as<float>() + params["b"_key].as<float>();
  });
  proto.addMethod("subtract"_key, [](dynamic&, const dynamic& params, dynamic& result) {
    result["result"_key] = params["a"_key].as<float>() - params["b"_key].as<float>();
  });
  proto.addMethod("multiply"_key, [](dynamic&, const dynamic& params, dynamic& result) {
    result["result"_key] = params["a"_key].as<float>() * params["b"_key].as<float>();
  });
  proto.addMethod("divide"_key, [](dynamic&, const dynamic& params, dynamic& result) {
    float a = params["a"_key].as<float>();
    float b = params["b"_key].as<float>();
    if (b == 0.0f) {
      result["error"_key] = std::string{"division by zero"};
      result["result"_key] = 0.0f;
    } else {
      result["result"_key] = a / b;
    }
  });
  dynamic::addClass(proto);
}

// bdg::bison::abi::key_t must stay qualified here: glibc's <sys/types.h>
// (pulled in transitively) also defines a global `key_t` typedef, and the
// `using namespace bdg::bison::abi;` above makes a bare `key_t` ambiguous
// between the two (same pitfall the internal C++ codebase's own tests
// document -- see tests/bison_c_tests.cpp).
float call(rmi::proxy& calc, bdg::bison::abi::key_t method, float a, float b) {
  dynamic params;
  params["a"_key] = a;
  params["b"_key] = b;
  dynamic result = calc.call(method, params);
  return result["result"_key].as<float>();
}

void run_client(int client_id) {
  // Each client gets its own standalone in-process connection.
  auto client = rmi::client::standalone();
  client.connect(); // no-op for standalone

  rmi::proxy calc = client.instantiate("Calculator"_key);
  {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    printf("[Client %d] connected\n", client_id);
  }

  float a = 10.0f * client_id;
  float add_result = call(calc, "add"_key, a, 3.0f);
  {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    printf("[Client %d] add(%.0f, 3) = %.0f\n", client_id, a, add_result);
  }

  float b = 7.0f * client_id;
  float sub_result = call(calc, "subtract"_key, 100.0f, b);
  {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    printf("[Client %d] subtract(100, %.0f) = %.0f\n", client_id, b, sub_result);
  }

  float v = static_cast<float>(client_id);
  float mul_result = call(calc, "multiply"_key, v, v);
  {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    printf("[Client %d] multiply(%.0f, %.0f) = %.0f\n", client_id, v, v, mul_result);
  }

  float div_result = call(calc, "divide"_key, 42.0f, v); // v is non-zero since client_id >= 1
  {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    printf("[Client %d] divide(42, %.0f) = %.0f\n", client_id, v, div_result);
  }

  calc.destroy();
  client.disconnect(); // no-op for standalone

  std::lock_guard<std::mutex> lock(g_print_mutex);
  printf("[Client %d] done.\n", client_id);
}

} // namespace

int main() {
  register_calculator();
  printf("[Server] RMI Calculator registered (standalone in-process mode).\n");

  constexpr int kNumClients = 3;
  std::vector<std::thread> threads;
  threads.reserve(kNumClients);
  for (int i = 1; i <= kNumClients; ++i)
    threads.emplace_back(run_client, i);

  for (auto& t : threads)
    t.join();

  printf("[Server] all clients done.\n");
  return 0;
}
