// MIT License © 2025 Binary Dice Games
// examples/rmi_stdio_server_example.cpp
//
// RMI server example using pty_server_app: owns a bash subprocess via forkpty
// and serves a Calculator over DCS-framed bison.  Run this on the server
// machine, then open a terminal session to it and run rmi_stdio_client_example.
//
// Linux only.

#if defined(__linux__)

#include "src/pty/pty_server_app.hpp"

#include <iostream>

using namespace bdg::bison;

class calculator_stdio_server final : public pty::pty_server_app {
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
        "divide"_key,
        [](dynamic& /*self*/, const dynamic& params) -> dynamic {
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

    dynamic::addClass(0U, proto, 0U);
  }

  void on_client_connected() const override {
    std::cerr << "[Server] client connected.\n";
  }

  void on_session_ended() const override {
    std::cerr << "[Server] session ended.\n";
  }
};

int main(int argc, char** argv) {
  calculator_stdio_server app;
  return app.run(argc, argv);
}

#else

#include <iostream>

int main() {
  std::cerr << "rmi_stdio_server_example is only supported on Linux.\n";
  return 1;
}

#endif // defined(__linux__)
