// MIT License © 2025 Binary Dice Games
/**
 * @file client_app.cpp
 * @brief Generic multi-transport client application scaffold implementation.
 */
#include "src/app/client/client_app.hpp"

#include "src/app/debugger.hpp"
#include "src/app/transport_flags.hpp"
#include "src/pty/raw_mode_guard.hpp"
#include "src/rmi/client/client.hpp"
#include "src/rmi/transport/named_pipe_transport.hpp"
#include "src/rmi/transport/socket_transport.hpp"
#include "src/rmi/transport/term_transport.hpp"

#include <gflags/gflags.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

DECLARE_string(host);
DECLARE_int32(port);
DECLARE_string(name);
DECLARE_int32(timeout);
DECLARE_bool(debugger);

namespace bdg::bison::app {

// ── Default hook implementations ──────────────────────────────────────────────

void client_app::on_connect_params(bison::dynamic& params) const {
  params["timeout_ms"_key] = int32_t{static_cast<int32_t>(timeout_.count())};
}

void client_app::on_error(const std::string& msg) const {
  std::cerr << "[client_app] error: " << msg << '\n';
}

// ── Console input ──────────────────────────────────────────────────────────────

bool client_app::read_console_line(std::string& line) {
  return static_cast<bool>(std::getline(std::cin, line));
}

// ── run_with_transport ────────────────────────────────────────────────────────

int client_app::run_with_transport(std::unique_ptr<rmi::transport::client_transport_iface> transport) {
  rmi::client c{std::move(transport)};

  bison::dynamic params;
  on_connect_params(params);
  c.connect(std::move(params));
  on_connected();

  const int result = on_session(c);
  c.disconnect();
  return result;
}

// ── run — flag parsing and transport selection ────────────────────────────────

int client_app::run(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_debugger) {
    wait_for_debugger();
  }

  timeout_ = std::chrono::milliseconds{FLAGS_timeout};

  try {
    const transport_kind transport = selected_transport();

    switch (transport) {
      case transport_kind::pipe:
        return run_with_transport(std::make_unique<rmi::transport::named_pipe_client_transport>(FLAGS_name));
      case transport_kind::tcp:
        return run_with_transport(
            std::make_unique<rmi::transport::socket_client_transport>(FLAGS_host, static_cast<uint16_t>(FLAGS_port)));
      case transport_kind::term: {
        pty::raw_mode_guard raw{0};
        return run_with_transport(std::make_unique<rmi::transport::term_client_transport>(0, 1));
      }
    }
    return 1;

  } catch (const std::exception& ex) {
    on_error(ex.what());
    return 1;
  } catch (...) {
    on_error("unexpected failure");
    return 1;
  }
}

} // namespace bdg::bison::app
