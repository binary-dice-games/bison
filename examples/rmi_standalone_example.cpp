// MIT License © 2025 Binary Dice Games
// examples/rmi_standalone_example.cpp
//
// Standalone (in-process) RMI example built on the standalone_app scaffold.
// Native-C++ counterpart to rmi_abi_standalone_example.cpp, which
// demonstrates the same rmi_standalone_create() C ABI entry point.
//
// Topology:
//   - No separate server process and no transport. standalone_app registers
//     a "Calculator" class and drives an rmi::standalone session directly
//     against the local class registry.

#include "src/app/standalone/standalone_app.hpp"
#include "src/bison/bison_flags.hpp"
#include "src/rmi/rmi.hpp"

#include <gflags/gflags.h>

#include <iostream>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::shared::constants;

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

class calculator_standalone_app : public app::standalone_app {
 protected:
  void register_classes() override {
    register_calculator();
  }

  int on_session(standalone& sa) override {
    auto calc = sa.instantiate(0U, "Calculator"_key).get();
    std::cout << "[Standalone] connected, object id=" << calc.object_id() << '\n';

    {
      dynamic params;
      params["a"_key] = 10.0f;
      params["b"_key] = 3.0f;
      auto result = calc.call("add"_key, std::move(params)).get();
      float res = result["result"_key];
      std::cout << "[Standalone] add(10, 3) = " << res << '\n';
    }

    {
      dynamic params;
      params["a"_key] = 100.0f;
      params["b"_key] = 21.0f;
      auto result = calc.call("subtract"_key, std::move(params)).get();
      float res = result["result"_key];
      std::cout << "[Standalone] subtract(100, 21) = " << res << '\n';
    }

    {
      dynamic params;
      params["a"_key] = 7.0f;
      params["b"_key] = 6.0f;
      auto result = calc.call("multiply"_key, std::move(params)).get();
      float res = result["result"_key];
      std::cout << "[Standalone] multiply(7, 6) = " << res << '\n';
    }

    {
      dynamic params;
      params["a"_key] = 42.0f;
      params["b"_key] = 2.0f;
      auto result = calc.call("divide"_key, std::move(params)).get();
      float res = result["result"_key];
      std::cout << "[Standalone] divide(42, 2) = " << res << '\n';
    }

    sa.destroy(std::move(calc));
    std::cout << "[Standalone] done." << '\n';
    return 0;
  }
};

} // namespace

int main(int argc, char** argv) {
  if (bdg::bison::print_usage(argc, argv, "Standalone (in-process) RMI Calculator example.", __FILE__))
    return 0;

  calculator_standalone_app app;
  return app.run(argc, argv);
}
