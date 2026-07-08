// MIT License © 2025 Binary Dice Games
/**
 * @file server_app.cpp
 * @brief Extensible server application scaffold implementation.
 */
#include "src/app/server/server_app.hpp"

#include "src/app/debugger.hpp"
#include "src/app/transport_flags.hpp"
#include "src/rmi/server/server.hpp"
#include "src/rmi/transport/named_pipe_transport.hpp"
#include "src/rmi/transport/socket_transport.hpp"
#include "src/rmi/transport/term_transport.hpp"
#include "src/term/terminal.hpp"

#include <gflags/gflags.h>

#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

DECLARE_string(host);
DECLARE_int32(port);
DECLARE_string(name);
DECLARE_string(cmd);
DECLARE_bool(verbose);
DECLARE_bool(debugger);

namespace bdg::bison::app {

// ── Internal server subclass that bridges rmi hooks to server_app hooks ───────

namespace {

class bridged_server : public rmi::server {
 public:
  bridged_server(rmi::transport::server_transport_iface& t, server_app& app) : rmi::server(t), app_(app) {}

 protected:
  void on_session_created(rmi::context& ctx) override {
    // Call the app hook first so wish can initialise the session logger before
    // the trace fires.
    app_.on_session_created(ctx);
    std::ostringstream oss;
    oss << "[verbose] open        sid=0x" << std::hex << std::setw(8) << std::setfill('0') << ctx.session_id.id;
    on_print(ctx.session_id, oss.str());
  }

  void on_session_destroyed(rmi::context& ctx) override {
    std::ostringstream oss;
    oss << "[verbose] close       sid=0x" << std::hex << std::setw(8) << std::setfill('0') << ctx.session_id.id << " ("
        << std::dec << ctx.objects.size() << " objects)";
    on_print(ctx.session_id, oss.str());
    app_.on_session_destroyed(ctx);
  }

  void on_print(bison::key_t session_id, const std::string& line) override {
    if (FLAGS_verbose)
      app_.on_verbose_trace(session_id, line);
  }

  std::string on_help_text() const override {
    return app_.server_description();
  }

 private:
  server_app& app_;
};

} // namespace

// ── Default hook implementations ──────────────────────────────────────────────

void server_app::on_verbose_trace(bison::key_t /*session_id*/, const std::string& line) const {
  // term::terminal (constructed for transport_kind::term in run()) already
  // redirects stdout through a CRLF-safe translator for its lifetime, so no
  // transport-specific handling is needed here anymore.
  std::cout << line << '\n';
}

void server_app::on_listening() const {
  switch (selected_transport()) {
    case transport_kind::pipe:
      std::cout << "[server_app] listening on pipe " << FLAGS_name << " -- press Enter to stop\n" << std::flush;
      return;
    case transport_kind::tcp:
      std::cout << "[server_app] listening on " << FLAGS_host << ':' << FLAGS_port << " -- press Enter to stop\n"
                << std::flush;
      return;
    case transport_kind::term:
      std::cout << "[server_app] listening via --transport=term -- exit the spawned terminal to stop\n" << std::flush;
      return;
  }
}

void server_app::on_session_created(rmi::context& ctx) const {
  (void)ctx;
}

void server_app::on_session_destroyed(rmi::context& ctx) const {
  (void)ctx;
}

void server_app::on_error(const std::string& msg) const {
  std::cerr << "[server_app] error: " << msg << '\n';
}

// ── Default run_with_transport — socket/stream sessions ──────────────────────

int server_app::run_with_transport(
    rmi::transport::server_transport_iface& transport,
    std::function<void()> wait_for_shutdown,
    std::function<bool()> /*is_shutdown_requested*/) {
  bridged_server srv{transport, *this};
  srv.listen();
  on_listening();
  if (wait_for_shutdown) {
    wait_for_shutdown();
  } else {
    std::string line;
    std::getline(std::cin, line);
  }
  srv.stop();
  return 0;
}

// ── run() — argument parsing and lifecycle ────────────────────────────────────

int server_app::run(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_debugger) {
    wait_for_debugger();
  }

  try {
    const transport_kind transport = selected_transport();

    register_classes();

    switch (transport) {
      case transport_kind::pipe: {
        rmi::transport::named_pipe_server_transport pipe_transport{FLAGS_name};
        return run_with_transport(pipe_transport);
      }
      case transport_kind::tcp: {
        auto port = static_cast<uint16_t>(FLAGS_port);
        rmi::transport::socket_server_transport socket_transport{FLAGS_host, port};
        return run_with_transport(socket_transport);
      }
      case transport_kind::term: {
        term::terminal term_proc{FLAGS_cmd};
        term_proc.start_pump();
        rmi::transport::term_server_transport term_transport{term_proc.read_handle(), term_proc.write_handle()};
        return run_with_transport(
            term_transport, [&] { term_proc.wait(); }, [&] { return term_proc.has_exited(); });
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
