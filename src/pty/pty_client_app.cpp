// MIT License © 2025 Binary Dice Games
/**
 * @file pty_client_app.cpp
 * @brief Remote PTY client application scaffold implementation.
 */
#include "src/pty/pty_client_app.hpp"

#if defined(__linux__)

#include "src/rmi/transport/stdio_transport.hpp"

#include <iostream>

namespace bdg::bison::pty {

// ── Default hook implementations ──────────────────────────────────────────

void pty_client_app::on_connected() const {}

void pty_client_app::on_error(const std::string& msg) const {
  std::cerr << "[pty_client_app] error: " << msg << '\n';
}

void pty_client_app::on_connect_params(bison::dynamic& params) const {
  params["mode"_key]                  = std::string{"dcs"};
  // Five minutes: gives the user time to SSH and start the client process
  // before the server's HELLO window expires.
  params["handshake_timeout_ms"_key]  = int32_t{300000};
}

// ── run() ─────────────────────────────────────────────────────────────────

int pty_client_app::run(int argc, char** argv) {
  (void)argc;
  (void)argv;

  try {
    // The client is a remote process whose stdin/stdout are the SSH channel.
    rmi::transport::stdio_client_transport transport;
    rmi::client c{std::move(transport)};

    bison::dynamic params;
    on_connect_params(params);

    c.connect(std::move(params));
    on_connected();

    const int result = on_session(c);
    c.disconnect();
    return result;
  } catch (const std::exception& ex) {
    on_error(ex.what());
    return 1;
  } catch (...) {
    on_error("unexpected failure");
    return 1;
  }
}

} // namespace bdg::bison::pty

#endif // defined(__linux__)
