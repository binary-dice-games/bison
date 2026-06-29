// MIT License © 2025 Binary Dice Games
// examples/rmi_server_example.cpp
//
// Standalone RMI server example using socket transport.

#include "src/rmi/rmi.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;
using namespace bdg::bison::rmi::shared::constants;

static void register_calculator() {
  auto proto = dynamic_ptr{"Calculator"_key, {}};

  proto->addMethod("add"_key, method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
                     float a = params["a"_key];
                     float b = params["b"_key];
                     dynamic result;
                     result["result"_key] = a + b;
                     return result;
                   }});

  proto->addMethod("subtract"_key, method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
                     float a = params["a"_key];
                     float b = params["b"_key];
                     dynamic result;
                     result["result"_key] = a - b;
                     return result;
                   }});

  proto->addMethod("multiply"_key, method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
                     float a = params["a"_key];
                     float b = params["b"_key];
                     dynamic result;
                     result["result"_key] = a * b;
                     return result;
                   }});

  proto->addMethod("divide"_key, method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
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
                   }});

  dynamic::addClass(0U, proto, 0U);
}

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 7070;

  if (argc > 1) {
    host = argv[1];
  }
  if (argc > 2) {
    const auto parsed = std::strtoul(argv[2], nullptr, 10);
    if (parsed > 0 && parsed <= 65535) {
      port = static_cast<uint16_t>(parsed);
    }
  }

  register_calculator();

  socket_server_transport transport{host, port};
  server srv{transport};
  srv.listen();

  std::cout << "[Server] Calculator listening on " << host << ":" << port << '\n';
  std::cout << "[Server] Press Enter to stop..." << '\n';

  std::string line;
  std::getline(std::cin, line);

  srv.stop();
  std::cout << "[Server] stopped." << '\n';

  return 0;
}
