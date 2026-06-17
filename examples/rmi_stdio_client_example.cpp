// MIT License © 2025 Binary Dice Games
// examples/rmi_stdio_client_example.cpp
//
// RMI client example using pty_client_app: runs inside the bash session opened
// by rmi_stdio_server_example and calls Calculator methods over DCS-framed
// bison transported through the terminal channel.
//
// Linux only.

#if defined(__linux__)

#include "src/pty/pty_client_app.hpp"

#include <iostream>

using namespace bdg::bison;

class calculator_stdio_client final : public pty::pty_client_app {
 protected:
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
      params["a"_key] = 21.0f;
      params["b"_key] = 7.0f;
      auto result = calc.call("divide"_key, std::move(params)).get();
      std::cerr << "[Client] divide(21, 7) = " << float(result["result"_key])
                << '\n';
    }

    c.destroy(std::move(calc));
    std::cerr << "[Client] done.\n";
    return 0;
  }
};

int main(int argc, char** argv) {
  calculator_stdio_client app;
  return app.run(argc, argv);
}

#else

#include <iostream>

int main() {
  std::cerr << "rmi_stdio_client_example is only supported on Linux.\n";
  return 1;
}

#endif // defined(__linux__)
