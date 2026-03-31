// MIT License © 2025 Binary Dice Games
// examples/rmi_stdio_server_example.cpp
//
// Standalone RMI server example built on reusable PTY server app scaffolding.

#include "src/rmi/rmi.hpp"

using namespace bdg::bison;
using namespace bdg::bison::rmi;

class calculator_pty_server_app final : public apps::pty_server_application {
 protected:
  void register_classes() override {
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
        "subtract"_key,
        [](dynamic& /*self*/, const dynamic& params) -> dynamic {
          float a = params["a"_key];
          float b = params["b"_key];
          dynamic result;
          result["result"_key] = a - b;
          return result;
        });

    proto->addMethod(
        "multiply"_key,
        [](dynamic& /*self*/, const dynamic& params) -> dynamic {
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
};

int main(int argc, char** argv) {
  calculator_pty_server_app app;
  return app.run(argc, argv);
}
