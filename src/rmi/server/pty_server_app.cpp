// MIT License © 2025 Binary Dice Games
/**
 * @file pty_server_app.cpp
 * @brief Reusable PTY server application scaffold implementation.
 */
#include "src/rmi/server/pty_server_app.hpp"

#include "src/rmi/transport/stdio_transport.hpp"

#include <chrono>
#include <iostream>

namespace bdg::bison::rmi::apps {

void pty_server_application::on_listen_params(bison::dynamic& params) const {
  params["mode"_key] = std::string{"dcs"};
}

void pty_server_application::on_listening() const {
  std::cerr << "[Server] stdio transport listening.\n";
}

void pty_server_application::on_waiting_for_disconnect() const {
  std::cerr << "[Server] Waiting for remote disconnect/end...\n";
}

void pty_server_application::on_stopped() const {
  std::cerr << "[Server] stopped.\n";
}

void pty_server_application::on_error(const std::string& message) const {
  std::cerr << "[Server] failed: " << message << '\n';
}

int pty_server_application::run(int argc, char** argv) {
  (void)argc;
  (void)argv;

  try {
    register_classes();

    transport::stdio_server_transport transport;
    server srv{transport};

    bison::dynamic params;
    on_listen_params(params);

    srv.listen(std::move(params));
    on_listening();
    on_waiting_for_disconnect();

    while (!transport.wait_until_closed(std::chrono::milliseconds{200})) {
    }

    srv.stop();
    on_stopped();
    return 0;
  } catch (const std::exception& ex) {
    on_error(ex.what());
    return 1;
  } catch (...) {
    on_error("unexpected failure");
    return 1;
  }
}

} // namespace bdg::bison::rmi::apps
