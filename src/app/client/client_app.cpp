// MIT License © 2025 Binary Dice Games
/**
 * @file client_app.cpp
 * @brief Generic multi-transport client application scaffold implementation.
 */
#include "src/app/client/client_app.hpp"

#include "src/app/debugger.hpp"
#include "src/app/transport_flags.hpp"
#include "src/rmi/client/client.hpp"
#include "src/rmi/transport/named_pipe_transport.hpp"
#include "src/rmi/transport/socket_transport.hpp"
#include "src/rmi/transport/term_transport.hpp"
#include "src/rmi/transport/tls_socket_transport.hpp"
#include "src/term/scoped_terminal_config.hpp"

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
DECLARE_string(ca_file);
DECLARE_string(ca_pem);
DECLARE_string(server_name);
DECLARE_bool(insecure_skip_verify);
DECLARE_string(cert_file);
DECLARE_string(cert_pem);
DECLARE_string(key_file);
DECLARE_string(key_pem);
DECLARE_string(key_password);

namespace bdg::bison::app {

// ── Default hook implementations ──────────────────────────────────────────────

void client_app::on_connect_params(bison::dynamic& params) const {
  params["timeout_ms"_key] = int32_t{static_cast<int32_t>(timeout_.count())};
  if (selected_transport() != transport_kind::tls)
    return;
  // Only set server_name when --server_name is explicitly non-empty: an
  // empty value here would override tls_socket_client_transport::open()'s
  // own fallback (defaults to --host) with a literal empty hostname, which
  // mbedTLS rejects as "verify without an expected hostname" instead of
  // silently falling back itself.
  if (!FLAGS_server_name.empty())
    params["server_name"_key] = FLAGS_server_name;
  params["ca_file"_key] = FLAGS_ca_file;
  params["ca_pem"_key] = FLAGS_ca_pem;
  params["insecure_skip_verify"_key] = FLAGS_insecure_skip_verify;
  params["cert_file"_key] = FLAGS_cert_file;
  params["cert_pem"_key] = FLAGS_cert_pem;
  params["key_file"_key] = FLAGS_key_file;
  params["key_pem"_key] = FLAGS_key_pem;
  params["key_password"_key] = FLAGS_key_password;
}

std::unique_ptr<rmi::client> client_app::make_client(
    std::unique_ptr<rmi::transport::client_transport_iface> transport) const {
  return std::make_unique<rmi::client>(std::move(transport));
}

void client_app::on_error(const std::string& msg) const {
  std::cerr << "[client_app] error: " << msg << '\n';
}

// ── Console input ─────────────────────────────────────────────────────────────

bool client_app::read_console_line(std::string& line) {
  return static_cast<bool>(std::getline(std::cin, line));
}

// ── run_with_transport ────────────────────────────────────────────────────────

int client_app::run_with_transport(std::unique_ptr<rmi::transport::client_transport_iface> transport) {
  std::unique_ptr<rmi::client> c = make_client(std::move(transport));

  bison::dynamic params;
  on_connect_params(params);
  c->connect(std::move(params));
  on_connected();

  const int result = on_session(*c);
  c->disconnect();
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
      case transport_kind::tls:
        return run_with_transport(std::make_unique<rmi::transport::tls_socket_client_transport>(
            FLAGS_host, static_cast<uint16_t>(FLAGS_port)));
      case transport_kind::term: {
        term::scoped_terminal_config stc{{0, 1}};
        auto term_transport = std::make_unique<rmi::transport::term_client_transport>(
            stc.upstream_read_fd(),
            stc.upstream_write_fd(),
            [&stc](std::string_view s) { stc.on_passthrough(s); },
            rmi::transport::kDefaultHandshakeTimeout,
            [&stc] { stc.stop_output_pump(); });
        stc.set_output_channel([raw = term_transport.get()](std::string_view s) { raw->send(s); });
        return run_with_transport(std::move(term_transport));
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
