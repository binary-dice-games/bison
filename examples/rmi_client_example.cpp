// MIT License © 2025 Binary Dice Games
// examples/rmi_client_example.cpp
//
// Standalone RMI client example using socket transport.

#include "src/rmi/rmi.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;
using namespace bdg::bison::rmi::shared::constants;

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
    socket_client_transport transport{host, port};
    client c{std::move(transport)};
    c.connect();

    auto calc = c.instantiate(0U, "Calculator"_key).get();
    std::cout << "[Client] connected, object id=" << calc.object_id() << '\n';

    {
      dynamic params;
      params["a"_key] = 10.0f;
      params["b"_key] = 3.0f;
      auto result = calc.call("add"_key, std::move(params)).get();
      float res = result["result"_key];
      std::cout << "[Client] add(10, 3) = " << res << '\n';
    }

    {
      dynamic params;
      params["a"_key] = 100.0f;
      params["b"_key] = 21.0f;
      auto result = calc.call("subtract"_key, std::move(params)).get();
      float res = result["result"_key];
      std::cout << "[Client] subtract(100, 21) = " << res << '\n';
    }

    {
      dynamic params;
      params["a"_key] = 7.0f;
      params["b"_key] = 6.0f;
      auto result = calc.call("multiply"_key, std::move(params)).get();
      float res = result["result"_key];
      std::cout << "[Client] multiply(7, 6) = " << res << '\n';
    }

    {
      dynamic params;
      params["a"_key] = 42.0f;
      params["b"_key] = 2.0f;
      auto result = calc.call("divide"_key, std::move(params)).get();
      float res = result["result"_key];
      std::cout << "[Client] divide(42, 2) = " << res << '\n';
    }

    c.destroy(std::move(calc));
    c.disconnect();
    std::cout << "[Client] done." << '\n';
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
