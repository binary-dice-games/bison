// MIT License © 2025 Binary Dice Games
// examples/rmi_server_abi_example.cpp
//
// RMI server example using the C ABI (bison_abi.dll) via the C++ RAII wrappers
// in include/rmi_c.hpp and include/bison_c.hpp.  Mirrors rmi_server_example.cpp
// but links only against bison_abi instead of the static bison library.

#include "include/rmi_c.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace bdg::bison::abi;  // dynamic, ""_key
using namespace bdg::rmi::abi;    // server

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

  auto srv = server::tcp(host.c_str(), port);
  srv.listen();

  std::cout << "[Server] Calculator listening on " << host << ":" << port
            << '\n';
  std::cout << "[Server] Press Enter to stop...\n";

  std::string line;
  std::getline(std::cin, line);

  srv.stop();
  std::cout << "[Server] stopped.\n";

  return 0;
}
