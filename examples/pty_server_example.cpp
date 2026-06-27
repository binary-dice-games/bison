// MIT License © 2025 Binary Dice Games
// examples/pty_server_example.cpp
//
// RMI server example using pty_server_app.  Spawns pty_client_example as a
// child process via uv_spawn(), serving a Calculator object over bison
// 4-byte-length-prefix framing on the child's stdin/stdout pipes.
//
// Usage (run from the directory containing both executables):
//   ./pty_server_example        (Linux / macOS)
//   .\pty_server_example.exe    (Windows)
//
// Cross-platform (Windows and Linux).

#include "src/app/pty/pty_server_app.hpp"

#include <iostream>

using namespace bdg::bison;

class calculator_pty_server final : public app::pty_server_app {
 protected:
  std::string shell_command() const override {
#if defined(_WIN32)
    return "pty_client_example.exe";
#else
    return "./pty_client_example";
#endif
  }

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
    std::cerr << "[Server] session ended.\n";
  }
};

int main(int argc, char** argv) {
  std::cerr << "[Server] starting PTY server — spawning client subprocess.\n";
  calculator_pty_server app;
  return app.run(argc, argv);
}
