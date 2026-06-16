// MIT License © 2025 Binary Dice Games
// examples/rmi_client_abi_example.cpp
//
// RMI client example using the C ABI (bison_abi.dll) via the C++ RAII wrappers
// in include/rmi_c.hpp and include/bison_c.hpp.  Mirrors rmi_client_example.cpp
// but links only against bison_abi instead of the static bison library.
//
// Run rmi_server_abi_example first, then this client.

#include "include/rmi_c.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

using namespace bdg::bison::abi;  // dynamic, ""_key
using namespace bdg::rmi::abi;    // client, proxy

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

  try {
    auto c = client::tcp(host.c_str(), port);
    c.connect();

    {
      auto calc = c.instantiate("Calculator"_key);
      std::cout << "[Client] connected\n";

      {
        auto params = dynamic::create();
        params.set("a"_key, 10.0f);
        params.set("b"_key, 3.0f);
        auto result = calc.call("add"_key, params);
        std::cout << "[Client] add(10, 3) = "
                  << result.get<float>("result"_key) << '\n';
      }

      {
        auto params = dynamic::create();
        params.set("a"_key, 100.0f);
        params.set("b"_key, 21.0f);
        auto result = calc.call("subtract"_key, params);
        std::cout << "[Client] subtract(100, 21) = "
                  << result.get<float>("result"_key) << '\n';
      }

      {
        auto params = dynamic::create();
        params.set("a"_key, 7.0f);
        params.set("b"_key, 6.0f);
        auto result = calc.call("multiply"_key, params);
        std::cout << "[Client] multiply(7, 6) = "
                  << result.get<float>("result"_key) << '\n';
      }

      {
        auto params = dynamic::create();
        params.set("a"_key, 42.0f);
        params.set("b"_key, 2.0f);
        auto result = calc.call("divide"_key, params);
        std::cout << "[Client] divide(42, 2) = "
                  << result.get<float>("result"_key) << '\n';
      }
    }  // proxy destroyed here — sends destroy to server

    c.disconnect();
    std::cout << "[Client] done.\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "[Client] failed to connect or execute calls at " << host
              << ":" << port << ": " << ex.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "[Client] unexpected failure while connecting to " << host
              << ":" << port << '\n';
    return 1;
  }
}
