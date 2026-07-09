// MIT License © 2025 Binary Dice Games
// examples/rmi_server_example.cpp
//
// Standalone RMI server example built on the server_app scaffold (same base
// class used by calc-server, src/srv/calc/main.cpp), to demonstrate the
// intended way to build a bison RMI server. Takes the same --transport/
// --host/--port/--name/--cmd/--verbose/--debugger flags as calc-server so
// usage is consistent across the project -- see docs/examples.md for
// per-transport walkthroughs.

#include "src/app/server/server_app.hpp"
#include "src/bison/bison_flags.hpp"
#include "src/rmi/rmi.hpp"

#include <gflags/gflags.h>

#include <iostream>

using namespace bdg::bison;
using namespace bdg::bison::rmi::shared::constants;

DEFINE_string(transport, "term", "Transport to use: tcp, pipe, or term");
DEFINE_string(host, "0.0.0.0", "Bind host address (transport=tcp)");
DEFINE_int32(port, 7070, "Listen port (transport=tcp)");
DEFINE_string(name, "", "Named-pipe / Unix-socket path (transport=pipe)");
DEFINE_string(cmd, "", "Command to spawn (transport=term)");
DEFINE_bool(verbose, false, "Print request/response trace messages to stdout");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");

static void register_calculator() {
  auto proto = dynamic_ptr{"Calculator"_key, {}};

  proto->addMethod("add"_key, method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
                     float a = params["a"_key];
                     float b = params["b"_key];
                     dynamic result;
                     result["result"_key] = a + b;
                     return result;
                   }});

  proto->addMethod("subtract"_key, method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
                     float a = params["a"_key];
                     float b = params["b"_key];
                     dynamic result;
                     result["result"_key] = a - b;
                     return result;
                   }});

  proto->addMethod("multiply"_key, method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
                     float a = params["a"_key];
                     float b = params["b"_key];
                     dynamic result;
                     result["result"_key] = a * b;
                     return result;
                   }});

  proto->addMethod("divide"_key, method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
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

namespace {

/** @brief Registers the Calculator class; everything else uses server_app's defaults. */
class calculator_server_app : public app::server_app {
 protected:
  void register_classes() override {
    register_calculator();
  }

  std::string server_description() const override {
    return "Standalone RMI Calculator server example.";
  }
};

} // namespace

int main(int argc, char** argv) {
  if (bdg::bison::print_usage(argc, argv, "Standalone RMI Calculator server example.", __FILE__))
    return 0;

  calculator_server_app app;
  return app.run(argc, argv);
}
