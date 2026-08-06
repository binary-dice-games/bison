// MIT License © 2025 Binary Dice Games
/**
 * @file bridge_app.cpp
 * @brief Generic multi-transport bridge application scaffold implementation.
 */
#include "src/app/bridge/bridge_app.hpp"

#include "src/app/debugger.hpp"
#include "src/app/transport_flags.hpp"
#include "src/rmi/bridge/bridge.hpp"
#include "src/rmi/transport/named_pipe_transport.hpp"
#include "src/rmi/transport/socket_transport.hpp"
#include "src/rmi/transport/term_transport.hpp"
#include "src/rmi/transport/tls_socket_transport.hpp"
#include "src/term/scoped_terminal_config.hpp"
#include "src/term/terminal.hpp"

#include <gflags/gflags.h>

#include <iostream>
#include <stdexcept>
#include <string>

DECLARE_string(downstream_host);
DECLARE_int32(downstream_port);
DECLARE_string(downstream_name);
DECLARE_string(cmd);
DECLARE_int32(timeout);
DECLARE_bool(debugger);
DECLARE_string(downstream_cert_file);
DECLARE_string(downstream_cert_pem);
DECLARE_string(downstream_key_file);
DECLARE_string(downstream_key_pem);
DECLARE_string(downstream_key_password);
DECLARE_string(downstream_client_auth);
DECLARE_string(downstream_ca_file);
DECLARE_string(downstream_ca_pem);

DECLARE_string(upstream_host);
DECLARE_int32(upstream_port);
DECLARE_string(upstream_name);
DECLARE_string(upstream_server_name);
DECLARE_string(upstream_ca_file);
DECLARE_string(upstream_ca_pem);
DECLARE_bool(upstream_insecure_skip_verify);
DECLARE_string(upstream_cert_file);
DECLARE_string(upstream_cert_pem);
DECLARE_string(upstream_key_file);
DECLARE_string(upstream_key_pem);
DECLARE_string(upstream_key_password);

namespace bdg::bison::app {

// ── Internal bridge subclass that bridges rmi hooks to bridge_app hooks ───────

namespace {

class bridged_bridge : public rmi::bridge {
 public:
  bridged_bridge(
      rmi::transport::server_transport_iface& downstream,
      std::unique_ptr<rmi::transport::client_transport_iface> upstream_transport,
      bison::dynamic upstream_params,
      bridge_app& app)
      : rmi::bridge(downstream, std::move(upstream_transport), std::move(upstream_params)), app_(app) {}

 protected:
  void on_client_connected(rmi::context& ctx) override {
    app_.on_client_connected(ctx);
  }

  void on_client_disconnected(rmi::context& ctx) override {
    app_.on_client_disconnected(ctx);
  }

  std::string on_help_text() const override {
    return app_.bridge_description();
  }

 private:
  bridge_app& app_;
};

} // namespace

// ── Default hook implementations ──────────────────────────────────────────────

void bridge_app::on_upstream_connect_params(bison::dynamic& params) const {
  params["timeout_ms"_key] = int32_t{FLAGS_timeout};
  if (selected_upstream_transport() != transport_kind::tls)
    return;
  // Only set server_name when --upstream_server_name is explicitly
  // non-empty -- see client_app::on_connect_params()'s identical comment.
  if (!FLAGS_upstream_server_name.empty())
    params["server_name"_key] = FLAGS_upstream_server_name;
  params["ca_file"_key] = FLAGS_upstream_ca_file;
  params["ca_pem"_key] = FLAGS_upstream_ca_pem;
  params["insecure_skip_verify"_key] = FLAGS_upstream_insecure_skip_verify;
  params["cert_file"_key] = FLAGS_upstream_cert_file;
  params["cert_pem"_key] = FLAGS_upstream_cert_pem;
  params["key_file"_key] = FLAGS_upstream_key_file;
  params["key_pem"_key] = FLAGS_upstream_key_pem;
  params["key_password"_key] = FLAGS_upstream_key_password;
}

void bridge_app::on_downstream_listen_params(bison::dynamic& params) const {
  if (selected_downstream_transport() != transport_kind::tls)
    return;
  params["cert_file"_key] = FLAGS_downstream_cert_file;
  params["cert_pem"_key] = FLAGS_downstream_cert_pem;
  params["key_file"_key] = FLAGS_downstream_key_file;
  params["key_pem"_key] = FLAGS_downstream_key_pem;
  params["key_password"_key] = FLAGS_downstream_key_password;
  params["client_auth"_key] = FLAGS_downstream_client_auth;
  params["ca_file"_key] = FLAGS_downstream_ca_file;
  params["ca_pem"_key] = FLAGS_downstream_ca_pem;
}

void bridge_app::on_error(const std::string& msg) const {
  std::cerr << "[bridge_app] error: " << msg << '\n';
}

std::unique_ptr<rmi::bridge> bridge_app::make_bridge(
    rmi::transport::server_transport_iface& downstream_transport,
    std::unique_ptr<rmi::transport::client_transport_iface> upstream_transport,
    bison::dynamic upstream_params) {
  return std::make_unique<bridged_bridge>(
      downstream_transport, std::move(upstream_transport), std::move(upstream_params), *this);
}

void bridge_app::on_listening() const {
  std::string downstream_desc;
  switch (selected_downstream_transport()) {
    case transport_kind::pipe:
      downstream_desc = "pipe " + FLAGS_downstream_name;
      break;
    case transport_kind::tcp:
      downstream_desc = FLAGS_downstream_host + ":" + std::to_string(FLAGS_downstream_port);
      break;
    case transport_kind::tls:
      downstream_desc = FLAGS_downstream_host + ":" + std::to_string(FLAGS_downstream_port) + " (tls)";
      break;
    case transport_kind::term:
      downstream_desc = "--downstream_transport=term";
      break;
  }
  std::cout << "[bridge_app] listening on " << downstream_desc << " -- exit the spawned terminal to stop\n"
            << std::flush;
}

// ── run_with_transport ────────────────────────────────────────────────────────

int bridge_app::run_with_transport(
    rmi::transport::server_transport_iface& downstream_transport,
    std::unique_ptr<rmi::transport::client_transport_iface> upstream_transport) {
  bison::dynamic upstream_params;
  on_upstream_connect_params(upstream_params);

  std::unique_ptr<rmi::bridge> br =
      make_bridge(downstream_transport, std::move(upstream_transport), std::move(upstream_params));

  bison::dynamic downstream_params;
  on_downstream_listen_params(downstream_params);
  br->start(std::move(downstream_params));
  on_listening();
  wait_for_shutdown();
  br->stop();
  return 0;
}

void bridge_app::wait_for_shutdown() {
  active_term_->wait();
}

// ── run() — argument parsing and lifecycle ────────────────────────────────────

int bridge_app::run(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_debugger) {
    wait_for_debugger();
  }

  try {
    const transport_kind downstream_kind = selected_downstream_transport();
    const transport_kind upstream_kind = selected_upstream_transport();

    // Unless the downstream side is already `term` (which spawns its own
    // terminal below), spawn a separate anchor terminal up front: a
    // genuinely usable interactive shell, decoupled from whatever the
    // upstream side is doing, so the console stays available for the
    // operator to keep working (e.g. launching more clients) without
    // disturbing the upstream link. `term_server_transport` (the same
    // wrapper the downstream `term` case below uses) pumps the anchor's own
    // output back out through `std::cout` by default -- which, in the
    // upstream=term case below, is itself redirected by
    // `scoped_terminal_config` and relayed over the same upstream link, so
    // the anchor's shell is visible to the operator exactly like the
    // downstream-spawned terminal is. Built before the upstream switch so
    // its write handle is available to feed directly from the upstream
    // `term` case, if selected.
    std::unique_ptr<term::terminal> anchor_term;
    std::unique_ptr<rmi::transport::term_server_transport> anchor_transport;
    if (downstream_kind != transport_kind::term) {
      anchor_term = std::make_unique<term::terminal>(std::string{}, terminal_label());
      anchor_transport = std::make_unique<rmi::transport::term_server_transport>(
          anchor_term->read_handle(), anchor_term->write_handle());
      anchor_transport->start(bison::dynamic{});
      active_term_ = anchor_term.get();
    }

    std::unique_ptr<term::scoped_terminal_config> upstream_stc;
    std::unique_ptr<rmi::transport::client_transport_iface> upstream_transport;
    switch (upstream_kind) {
      case transport_kind::pipe:
        upstream_transport = std::make_unique<rmi::transport::named_pipe_client_transport>(FLAGS_upstream_name);
        break;
      case transport_kind::tcp:
        upstream_transport = std::make_unique<rmi::transport::socket_client_transport>(
            FLAGS_upstream_host, static_cast<uint16_t>(FLAGS_upstream_port));
        break;
      case transport_kind::tls:
        upstream_transport = std::make_unique<rmi::transport::tls_socket_client_transport>(
            FLAGS_upstream_host, static_cast<uint16_t>(FLAGS_upstream_port));
        break;
      case transport_kind::term: {
        upstream_stc = std::make_unique<term::scoped_terminal_config>(term::scoped_terminal_config::params{0, 1});
        term::scoped_terminal_config* stc = upstream_stc.get();
        rmi::transport::term_passthrough_cb passthrough;
        if (anchor_term) {
          // The anchor is a real pty running an interactive shell, so feed
          // it via on_terminal_passthrough() (raw, unbuffered) rather than
          // on_passthrough() (line-buffered, for a std::getline()-style
          // reader) -- see on_terminal_passthrough()'s doc comment.
          const int anchor_write_fd = anchor_term->write_handle();
          passthrough = [anchor_write_fd](std::string_view s) {
            term::scoped_terminal_config::on_terminal_passthrough(anchor_write_fd, s);
          };
        } else {
          passthrough = [stc](std::string_view s) { stc->on_passthrough(s); };
        }
        auto term_transport = std::make_unique<rmi::transport::term_client_transport>(
            stc->upstream_read_fd(),
            stc->upstream_write_fd(),
            std::move(passthrough),
            rmi::transport::kDefaultHandshakeTimeout,
            [stc] { stc->stop_output_pump(); });
        stc->set_output_channel([raw = term_transport.get()](std::string_view s) { raw->send(s); });
        upstream_transport = std::move(term_transport);
        break;
      }
    }

    if (anchor_term && upstream_kind != transport_kind::term) {
      // No upstream link is already feeding the anchor -- pump this
      // process's own (unclaimed) real stdin into it directly.
      anchor_term->start_pump();
    }

    switch (downstream_kind) {
      case transport_kind::pipe: {
        rmi::transport::named_pipe_server_transport pipe_transport{FLAGS_downstream_name};
        int rc = run_with_transport(pipe_transport, std::move(upstream_transport));
        active_term_ = nullptr;
        return rc;
      }
      case transport_kind::tcp: {
        auto port = static_cast<uint16_t>(FLAGS_downstream_port);
        rmi::transport::socket_server_transport socket_transport{FLAGS_downstream_host, port};
        int rc = run_with_transport(socket_transport, std::move(upstream_transport));
        active_term_ = nullptr;
        return rc;
      }
      case transport_kind::tls: {
        auto port = static_cast<uint16_t>(FLAGS_downstream_port);
        rmi::transport::tls_socket_server_transport tls_transport{FLAGS_downstream_host, port};
        int rc = run_with_transport(tls_transport, std::move(upstream_transport));
        active_term_ = nullptr;
        return rc;
      }
      case transport_kind::term: {
        term::terminal term_proc{FLAGS_cmd, terminal_label()};
        term_proc.start_pump();
        rmi::transport::term_server_transport term_transport{term_proc.read_handle(), term_proc.write_handle()};
        active_term_ = &term_proc;
        int rc = run_with_transport(term_transport, std::move(upstream_transport));
        active_term_ = nullptr;
        return rc;
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
