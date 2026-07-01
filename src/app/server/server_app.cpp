// MIT License © 2025 Binary Dice Games
/**
 * @file server_app.cpp
 * @brief Extensible server application scaffold implementation.
 */
#include "src/app/server/server_app.hpp"

#include "src/rmi/server/server.hpp"
#include "src/rmi/transport/named_pipe_transport.hpp"
#include "src/rmi/transport/socket_transport.hpp"

#if defined(__linux__) || defined(_WIN32)
#include <atomic>
#include <condition_variable>
#include <mutex>
#include "src/rmi/transport/pty_server_transport.hpp"
#include "src/rmi/transport/transport_iface.hpp"
#endif

#include <gflags/gflags.h>

#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

DECLARE_string(host);
DECLARE_int32(port);
DECLARE_string(pipe);
DECLARE_bool(verbose);
DECLARE_string(pty);
DECLARE_int32(pty_cols);
DECLARE_int32(pty_rows);

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
  std::cout << line << '\n';
}

void server_app::on_listening() const {
#if defined(__linux__) || defined(_WIN32)
  if (!FLAGS_pty.empty()) {
    std::cout << "[server_app] PTY transport: running " << FLAGS_pty
              << " -- press Enter to stop\n" << std::flush;
    return;
  }
#endif
  if (!FLAGS_pipe.empty()) {
    std::cout << "[server_app] listening on pipe " << FLAGS_pipe << " -- press Enter to stop\n" << std::flush;
  } else {
    std::cout << "[server_app] listening on " << FLAGS_host << ':' << FLAGS_port << " -- press Enter to stop\n"
              << std::flush;
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

int server_app::run_with_transport(rmi::transport::server_transport_iface& transport) {
  bridged_server srv{transport, *this};
  srv.listen();
  on_listening();
  std::string line;
  std::getline(std::cin, line);
  srv.stop();
  return 0;
}

// ── run() — argument parsing and lifecycle ────────────────────────────────────

int server_app::run(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  try {
    register_classes();

#if defined(__linux__) || defined(_WIN32)
    if (!FLAGS_pty.empty()) {
      rmi::pty::pty_config cfg;
      cfg.cmd = FLAGS_pty;
      cfg.cols = static_cast<uint16_t>(FLAGS_pty_cols);
      cfg.rows = static_cast<uint16_t>(FLAGS_pty_rows);
      // Route plain (non-DCS) child output to the server's stdout.
      rmi::transport::pty_server_transport transport{
          std::move(cfg),
          [](uint8_t b) { std::cout.put(static_cast<char>(b)); std::cout.flush(); }};
      return run_with_transport(transport);
    }
#endif

    if (!FLAGS_pipe.empty()) {
      rmi::transport::named_pipe_server_transport transport{FLAGS_pipe};
      return run_with_transport(transport);
    }

    auto port = static_cast<uint16_t>(FLAGS_port);
    rmi::transport::socket_server_transport transport{FLAGS_host, port};
    return run_with_transport(transport);
  } catch (const std::exception& ex) {
    on_error(ex.what());
    return 1;
  } catch (...) {
    on_error("unexpected failure");
    return 1;
  }
}

} // namespace bdg::bison::app
