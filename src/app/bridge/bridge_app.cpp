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

DECLARE_string(upstream_host);
DECLARE_int32(upstream_port);
DECLARE_string(upstream_name);

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
    case transport_kind::term:
      downstream_desc = "--downstream_transport=term";
      break;
  }
  std::cout << "[bridge_app] listening on " << downstream_desc << " -- press Enter to stop\n" << std::flush;
}

// ── run_with_transport ────────────────────────────────────────────────────────

int bridge_app::run_with_transport(
    rmi::transport::server_transport_iface& downstream_transport,
    std::unique_ptr<rmi::transport::client_transport_iface> upstream_transport) {
  bison::dynamic upstream_params;
  on_upstream_connect_params(upstream_params);

  std::unique_ptr<rmi::bridge> br =
      make_bridge(downstream_transport, std::move(upstream_transport), std::move(upstream_params));
  br->start();
  on_listening();
  wait_for_shutdown();
  br->stop();
  return 0;
}

void bridge_app::wait_for_shutdown() {
  if (active_term_) {
    active_term_->wait();
    return;
  }
  std::string line;
  std::getline(std::cin, line);
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
      case transport_kind::term: {
        upstream_stc = std::make_unique<term::scoped_terminal_config>(term::scoped_terminal_config::params{0, 1});
        term::scoped_terminal_config* stc = upstream_stc.get();
        auto term_transport = std::make_unique<rmi::transport::term_client_transport>(
            stc->upstream_read_fd(),
            stc->upstream_write_fd(),
            [stc](std::string_view s) { stc->on_passthrough(s); },
            rmi::transport::kDefaultHandshakeTimeout,
            [stc] { stc->stop_output_pump(); });
        stc->set_output_channel([raw = term_transport.get()](std::string_view s) { raw->send(s); });
        upstream_transport = std::move(term_transport);
        break;
      }
    }

    switch (downstream_kind) {
      case transport_kind::pipe: {
        rmi::transport::named_pipe_server_transport pipe_transport{FLAGS_downstream_name};
        return run_with_transport(pipe_transport, std::move(upstream_transport));
      }
      case transport_kind::tcp: {
        auto port = static_cast<uint16_t>(FLAGS_downstream_port);
        rmi::transport::socket_server_transport socket_transport{FLAGS_downstream_host, port};
        return run_with_transport(socket_transport, std::move(upstream_transport));
      }
      case transport_kind::term: {
        term::terminal term_proc{FLAGS_cmd};
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
