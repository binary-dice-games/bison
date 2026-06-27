// MIT License © 2025 Binary Dice Games
// examples/pty_server_example.cpp
//
// RMI server example using pty_server_app.  The server spawns a child process
// via uv_spawn() and communicates with it over stdin/stdout pipes using bison
// 4-byte-length-prefix framing.
//
// The child process can be anything that speaks bison pipe framing:
//
//   Local subprocess (demo, both sides in the same machine):
//     shell_command() → "./pty_client_example"
//
//   Remote subprocess over SSH (primary production use case):
//     shell_command() → "ssh user@remote-host ./pty_client_example"
//
//   The server spawns SSH, SSH tunnels the pipe frames transparently to
//   pty_client_example running on the remote machine.  SSH acts as a
//   binary-safe relay — no special configuration needed.
//
// Usage:
//   ./pty_server_example                                  (spawns local client)
//   ./pty_server_example --run cmd.exe                    (spawns cmd.exe)
//   ./pty_server_example --run "ssh user@host ./pty_client_example"
//
// Cross-platform (Windows and Linux).

#include "src/app/pty/pty_server_app.hpp"

#include <gflags/gflags.h>
#include <iostream>

DECLARE_string(run);

using namespace bdg::bison;

class calculator_pty_server final : public app::pty_server_app {
 protected:
  // Use --run "cmd" to set the child command at runtime, e.g.:
  //   --run cmd.exe
  //   --run "ssh user@host ./pty_client_example"
  std::string shell_command() const override {
    if (!FLAGS_run.empty()) return FLAGS_run;
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
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  std::cerr << "[Server] starting PTY server — spawning client subprocess.\n";
  calculator_pty_server app;
  return app.run(argc, argv);
}
