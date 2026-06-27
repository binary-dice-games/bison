// MIT License © 2025 Binary Dice Games
// examples/pty_client_example.cpp
//
// RMI client example using pty_client_app.  Reads frames from stdin and writes
// frames to stdout using 4-byte big-endian length-prefix framing.
//
// Designed to be launched by pty_server_example (directly or via SSH):
//
//   Direct spawn (same machine):
//     pty_server_example spawns ./pty_client_example via uv_spawn()
//
//   Remote spawn over SSH (primary production use case):
//     pty_server_example spawns "ssh user@this-host ./pty_client_example"
//     SSH connects the server's pipe to this binary's stdin/stdout
//     transparently — the 4-byte bison frames pass through SSH unchanged.
//
// When stdin is a pipe (spawned by server or SSH), pty_client_app uses
// uv_pipe_t automatically via uv_guess_handle().
//
// Cross-platform (Windows and Linux).

#include "src/app/pty/pty_client_app.hpp"

#include <iostream>

using namespace bdg::bison;

class calculator_pty_client final : public app::pty_client_app {
 protected:
  void on_connected() const override {
    std::cerr << "[Client] handshake complete — connected to Calculator server.\n";
  }

  int on_session(rmi::client& c) override {
    auto calc = c.instantiate(0U, "Calculator"_key).get();
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
      params["a"_key] = 100.0f;
      params["b"_key] = 21.0f;
      auto result = calc.call("subtract"_key, std::move(params)).get();
      std::cerr << "[Client] subtract(100, 21) = "
                << float(result["result"_key]) << '\n';
    }

    {
      dynamic params;
      params["a"_key] = 7.0f;
      params["b"_key] = 6.0f;
      auto result = calc.call("multiply"_key, std::move(params)).get();
      std::cerr << "[Client] multiply(7, 6) = " << float(result["result"_key])
                << '\n';
    }

    {
      dynamic params;
      params["a"_key] = 42.0f;
      params["b"_key] = 2.0f;
      auto result = calc.call("divide"_key, std::move(params)).get();
      std::cerr << "[Client] divide(42, 2) = " << float(result["result"_key])
                << '\n';
    }

    c.destroy(std::move(calc));
    std::cerr << "[Client] done.\n";
    return 0;
  }
};

int main(int argc, char** argv) {
  calculator_pty_client app;
  return app.run(argc, argv);
}
