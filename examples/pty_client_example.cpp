// MIT License © 2025 Binary Dice Games
// examples/pty_client_example.cpp
//
// PTY RMI client example using reusable pty_client_application.

#include "src/rmi/rmi.hpp"

#include <iostream>

using namespace bdg::bison;
using namespace bdg::bison::rmi;

class pty_test_client_app final : public apps::pty_client_application {
 protected:
  int on_session(client& rmi_client, const run_context& /*ctx*/) override {
    auto service = rmi_client.instantiate(0U, "PtyTestService"_key).get();

    dynamic params;
    params["message"_key] = std::string{"hello from pty_client_example"};

    auto result = service.call("ping"_key, std::move(params)).get();
    std::cerr << "[Client] ping reply: "
              << result["reply"_key].as<std::string>() << '\n';

    rmi_client.destroy(std::move(service));
    std::cerr << "[Client] test session done.\n";
    return 0;
  }

  void on_subprocess_started(const run_context& ctx) const override {
    if (ctx.mode == launch_mode::pty) {
      std::cerr << "[Client] terminal subprocess started.\n";
      std::cerr << "[Client] Start pty_server_example inside that terminal.\n";
    } else {
      std::cerr << "[Client] subprocess started.\n";
    }
  }
};

int main(int argc, char** argv) {
  pty_test_client_app app;
  return app.run(argc, argv);
}
