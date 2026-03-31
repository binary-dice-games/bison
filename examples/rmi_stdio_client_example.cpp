// MIT License © 2025 Binary Dice Games
// examples/rmi_stdio_client_example.cpp
//
// Standalone RMI client example built on reusable PTY client app scaffolding.

#include "src/rmi/rmi.hpp"

#include <iostream>

using namespace bdg::bison;
using namespace bdg::bison::rmi;

class calculator_pty_client_app final : public apps::pty_client_application {
 protected:
  int on_session(client& rmi_client, const run_context& /*ctx*/) override {
    auto calc = rmi_client.instantiate("Calculator"_key).get();
    std::cerr << "[Client] instantiated Calculator, id=" << calc.object_id()
              << '\n';

    {
      dynamic params;
      params["a"_key] = 10.0f;
      params["b"_key] = 3.0f;
      auto result = calc.call("add"_key, std::move(params)).get();
      std::cerr << "[Client] add(10, 3) = " << float(result["result"_key])
                << '\n';
    }

    {
      dynamic params;
      params["a"_key] = 21.0f;
      params["b"_key] = 7.0f;
      auto result = calc.call("divide"_key, std::move(params)).get();
      std::cerr << "[Client] divide(21, 7) = " << float(result["result"_key])
                << '\n';
    }

    rmi_client.destroy(std::move(calc));
    std::cerr << "[Client] done.\n";
    return 0;
  }
};

int main(int argc, char** argv) {
  calculator_pty_client_app app;
  return app.run(argc, argv);
}
