// MIT License © 2025 Binary Dice Games
/**
 * @file client_app.cpp
 * @brief Generic multi-transport client application scaffold implementation.
 */
#include "src/app/client/client_app.hpp"

#include "src/pty/raw_mode_guard.hpp"
#include "src/rmi/client/client.hpp"
#include "src/rmi/transport/named_pipe_transport.hpp"
#include "src/rmi/transport/socket_transport.hpp"
#include "src/rmi/transport/stdio_transport.hpp"

#include <gflags/gflags.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

extern void wait_for_debugger();

DECLARE_string(host);
DECLARE_int32(port);
DECLARE_string(pipe);
DECLARE_bool(pty);
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

// ── Console input (see read_console_line()'s doc comment for why) ─────────────

void client_app::feed_console_passthrough(std::string_view chunk) {
  console_queue_.withWLock([&](auto& st) {
    if (chunk.empty()) {
      st.closed = true;
      return;
    }
    st.partial.append(chunk.data(), chunk.size());
    size_t pos;
    while ((pos = st.partial.find('\n')) != std::string::npos) {
      st.lines.push(st.partial.substr(0, pos));
      st.partial.erase(0, pos + 1);
    }
  });
  console_queue_.notify_all();
}

bool client_app::read_console_line(std::string& line) {
  if (!console_via_passthrough_)
    return static_cast<bool>(std::getline(std::cin, line));

  bool got = false;
  console_queue_.wait([&](auto& st) {
    if (!st.lines.empty()) {
      line = std::move(st.lines.front());
      st.lines.pop();
      got = true;
      return true;
    }
    return st.closed;
  });
  return got;
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
    if (FLAGS_pty) {
      // fd 0/1 are a pty slave left in cooked mode (see src/pty/DESIGN.md):
      // its ONLCR/ECHO processing corrupts the BISON: line framing, so put
      // it in raw mode for the RMI session and restore it on the way out.
      pty::raw_mode_guard raw{0};
      // The transport's background reader owns fd 0 (non-blocking, scanning
      // for BISON: frames), so std::cin can't safely read it too — route
      // operator keystrokes through the passthrough callback instead; see
      // read_console_line().
      console_via_passthrough_ = true;
      return run_with_transport(std::make_unique<rmi::transport::stdio_client_transport>(
          0, 1, [this](std::string_view chunk) { feed_console_passthrough(chunk); }));
    }

    if (!FLAGS_pipe.empty()) {
      return run_with_transport(std::make_unique<rmi::transport::named_pipe_client_transport>(FLAGS_pipe));
    }

    return run_with_transport(
        std::make_unique<rmi::transport::socket_client_transport>(FLAGS_host, static_cast<uint16_t>(FLAGS_port)));

  } catch (const std::exception& ex) {
    on_error(ex.what());
    return 1;
  } catch (...) {
    on_error("unexpected failure");
    return 1;
  }
}

} // namespace bdg::bison::app
