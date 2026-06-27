// MIT License © 2025 Binary Dice Games
/**
 * @file pty_client_app.cpp
 * @brief PTY client application scaffold — delegates to client_app.
 */
#include "src/app/pty/pty_client_app.hpp"

#if defined(__linux__) || defined(_WIN32)

#include "src/rmi/transport/pty_client_transport.hpp"

#include <iostream>
#include <memory>

namespace bdg::bison::app {

void pty_client_app::on_connect_params(bison::dynamic& params) const {
  // Five minutes: gives the user time to start the client process inside the
  // server's terminal before the handshake window expires.
  params["handshake_timeout_ms"_key] = int32_t{300000};
}

void pty_client_app::on_error(const std::string& msg) const {
  std::cerr << "[pty_client_app] error: " << msg << '\n';
}

int pty_client_app::run(int argc, char** argv) {
  (void)argc;
  (void)argv;
  try {
    return run_with_transport(std::make_unique<pty_client_transport>());
  } catch (const std::exception& ex) {
    on_error(ex.what());
    return 1;
  } catch (...) {
    on_error("unexpected failure");
    return 1;
  }
}

} // namespace bdg::bison::app

#endif // defined(__linux__) || defined(_WIN32)
