// MIT License © 2025 Binary Dice Games
// examples/rmi_stdio_server_example.cpp
//
// Standalone RMI server example using stdio transport.

#include "src/rmi/rmi.hpp"

#include <chrono>
#include <iostream>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;

static void register_calculator() {
  auto proto = dynamic_ptr{"Calculator"_key, {}};

  proto->addMethod(
      "add"_key, [](dynamic& /*self*/, const dynamic& params) -> dynamic {
        float a = params["a"_key];
        float b = params["b"_key];
        dynamic result;
        result["result"_key] = a + b;
        return result;
      });

  proto->addMethod(
      "subtract"_key, [](dynamic& /*self*/, const dynamic& params) -> dynamic {
        float a = params["a"_key];
        float b = params["b"_key];
        dynamic result;
        result["result"_key] = a - b;
        return result;
      });

  proto->addMethod(
      "multiply"_key, [](dynamic& /*self*/, const dynamic& params) -> dynamic {
        float a = params["a"_key];
        float b = params["b"_key];
        dynamic result;
        result["result"_key] = a * b;
        return result;
      });

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

int main() {
  register_calculator();

  stdio_server_transport transport;
  server srv{transport};

  dynamic params;
  params["mode"_key] = std::string{"auto"};

  srv.listen(std::move(params));

  std::cerr << "[Server] stdio transport listening.\n";
  std::cerr << "[Server] Waiting for remote disconnect/end...\n";

  while (!transport.wait_until_closed(std::chrono::milliseconds{200})) {
  }

  srv.stop();
  std::cerr << "[Server] stopped.\n";

  return 0;
}
