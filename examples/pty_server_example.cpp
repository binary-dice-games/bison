// MIT License © 2025 Binary Dice Games
// examples/pty_server_example.cpp
//
// RMI server example using pty_server_app: owns a bash subprocess via forkpty,
// serves a Calculator over DCS-framed bison to any client that connects via SSH
// and runs pty_client_example in the resulting terminal.
//
// Linux only.

#if defined(__linux__)

#include "src/app/pty/pty_server_app.hpp"

#include <iostream>

using namespace bdg::bison;

class calculator_pty_server final : public app::pty_server_app {
 protected:
  void register_classes() override {
    auto proto = dynamic_ptr{"Calculator"_key, {}};

    proto->addMethod(
        "add"_key, method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
          float a = params["a"_key];
          float b = params["b"_key];
          dynamic result;
          result["result"_key] = a + b;
          return result;
        }});

    proto->addMethod(
        "subtract"_key,
        method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
          float a = params["a"_key];
          float b = params["b"_key];
          dynamic result;
          result["result"_key] = a - b;
          return result;
        }});

    proto->addMethod(
        "multiply"_key,
        method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
          float a = params["a"_key];
          float b = params["b"_key];
          dynamic result;
          result["result"_key] = a * b;
          return result;
        }});

    proto->addMethod(
        "divide"_key,
        method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
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

  void on_client_connected() const override {
    std::cerr << "[Server] bison client connected — serving Calculator.\n";
  }

  void on_session_ended() const override {
    std::cerr << "[Server] session ended — waiting for next client.\n";
  }
};

int main(int argc, char** argv) {
  std::cerr << "[Server] starting PTY server (bash subprocess via forkpty).\n";
  std::cerr << "[Server] SSH in and run pty_client_example to connect.\n";
  calculator_pty_server app;
  return app.run(argc, argv);
}

#else

#include <iostream>

int main() {
  std::cerr << "rmi_pty_server_example is only supported on Linux.\n";
  return 1;
}

#endif // defined(__linux__)
