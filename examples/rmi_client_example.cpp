// MIT License © 2025 Binary Dice Games
// examples/rmi_client_example.cpp
//
// Standalone RMI client example built on the client_app scaffold (same base
// class used by bison-cli, src/app/cli/main.cpp), to demonstrate the intended
// way to build a bison RMI client. Takes the same --transport/--host/--port/
// --name/--timeout/--debugger flags as bison-cli so usage is consistent
// across the project -- see docs/examples.md for per-transport walkthroughs.

#include "src/app/client/client_app.hpp"
#include "src/bison/bison_flags.hpp"
#include "src/rmi/rmi.hpp"

#include <gflags/gflags.h>

#include <iostream>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::shared::constants;

DEFINE_string(transport, "term", "Transport to use: tcp, pipe or term");
DEFINE_string(host, "127.0.0.1", "Server host address (transport=tcp)");
DEFINE_int32(port, 7070, "Server TCP port (transport=tcp)");
DEFINE_string(name, "", "Named-pipe / Unix-socket path (transport=pipe)");
DEFINE_int32(timeout, 30000, "Per-request timeout in milliseconds");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");

/** @brief Runs the calls shared by every transport against a connected client. */
static void run_calls(client& c) {
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
}

namespace {

/** @brief Runs the shared Calculator calls; everything else uses client_app's defaults. */
class calculator_client_app : public app::client_app {
 protected:
  int on_session(client& c) override {
    run_calls(c);
    std::cout << "[Client] done." << '\n';
    return 0;
  }

  void on_error(const std::string& msg) const override {
    std::cerr << "[Client] failed to connect or execute calls: " << msg << '\n';
  }
};

} // namespace

int main(int argc, char** argv) {
  if (bdg::bison::print_usage(argc, argv, "Standalone RMI Calculator client example.", __FILE__))
    return 0;

  calculator_client_app app;
  return app.run(argc, argv);
}
