// MIT License © 2025 Binary Dice Games
// examples/pty_server_example.cpp
//
// PTY RMI server example using reusable pty_server_application.

#include "src/rmi/rmi.hpp"

using namespace bdg::bison;
using namespace bdg::bison::rmi;

class pty_test_server_app final : public apps::pty_server_application {
 protected:
  void register_classes() override {
    auto proto = dynamic_ptr{"PtyTestService"_key, {}};

    proto->addMethod(
        "ping"_key, [](dynamic& /*self*/, const dynamic& params) -> dynamic {
          dynamic result;

          std::string message = "pong";
          if (const auto* msg = params.findField("message"_key);
              msg != nullptr && msg->is<std::string>()) {
            message = msg->as<std::string>();
          }

          result["reply"_key] = std::string{"pong: "} + message;
          return result;
        });

    dynamic::addClass(0U, proto);
  }
};

int main(int argc, char** argv) {
  pty_test_server_app app;
  return app.run(argc, argv);
}
