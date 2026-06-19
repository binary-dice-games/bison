// MIT License © 2025 Binary Dice Games
/**
 * @file srv_app.cpp
 * @brief Extensible server application scaffold implementation.
 */
#include "src/app/srv/srv_app.hpp"

#include "src/rmi/server/server.hpp"
#include "src/rmi/transport/socket_transport.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bdg::bison::app {

// ── Internal server subclass that bridges rmi hooks to srv_app hooks ─────────

namespace {

class bridged_server : public rmi::server {
 public:
  bridged_server(rmi::transport::server_transport_iface& t, srv_app& app)
      : rmi::server(t), app_(app) {}

 protected:
  void on_session_created(rmi::context& ctx) override {
    app_.on_session_created(ctx);
  }

  void on_session_destroyed(rmi::context& ctx) override {
    app_.on_session_destroyed(ctx);
  }

 private:
  srv_app& app_;
};

} // namespace

// ── Default hook implementations ──────────────────────────────────────────────

void srv_app::on_listening(const std::string& host, uint16_t port) const {
  std::cout << "[srv_app] listening on " << host << ':' << port
            << " — press Enter to stop\n"
            << std::flush;
}

void srv_app::on_session_created(rmi::context& ctx) const { (void)ctx; }

void srv_app::on_session_destroyed(rmi::context& ctx) const { (void)ctx; }

void srv_app::on_error(const std::string& msg) const {
  std::cerr << "[srv_app] error: " << msg << '\n';
}

// ── run() — argument parsing and lifecycle ────────────────────────────────────

int srv_app::run(int argc, char** argv) {
  std::string host = "0.0.0.0";
  uint16_t    port = 7070;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    if (arg == "--host") {
      if (i + 1 >= argc) { on_error("--host requires a value"); return 1; }
      host = argv[++i];
    } else if (arg == "--port") {
      if (i + 1 >= argc) { on_error("--port requires a value"); return 1; }
      port = static_cast<uint16_t>(std::stoi(argv[++i]));
    } else {
      on_error("unknown option: " + std::string(arg));
      return 1;
    }
  }

  try {
    register_classes();

    rmi::transport::socket_server_transport transport{host, port};
    bridged_server srv{transport, *this};

    srv.listen();
    on_listening(host, port);

    std::string line;
    std::getline(std::cin, line);

    srv.stop();
    return 0;
  } catch (const std::exception& ex) {
    on_error(ex.what());
    return 1;
  } catch (...) {
    on_error("unexpected failure");
    return 1;
  }
}

} // namespace bdg::bison::app
