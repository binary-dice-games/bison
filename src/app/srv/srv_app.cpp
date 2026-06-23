// MIT License © 2025 Binary Dice Games
/**
 * @file srv_app.cpp
 * @brief Extensible server application scaffold implementation.
 */
#include "src/app/srv/srv_app.hpp"

#include "src/rmi/server/server.hpp"
#include "src/rmi/transport/named_pipe_transport.hpp"
#include "src/rmi/transport/socket_transport.hpp"

#if defined(__linux__)
#  include "src/app/pty/pty_server_transport.hpp"
#  include "src/rmi/transport/transport_iface.hpp"
#  include <atomic>
#  include <condition_variable>
#  include <mutex>
#endif

#include <gflags/gflags.h>

#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

DECLARE_string(host);
DECLARE_int32 (port);
DECLARE_string(pipe);
DECLARE_bool  (pty);
DECLARE_bool  (verbose);

namespace bdg::bison::app {

// ── Internal server subclass that bridges rmi hooks to srv_app hooks ─────────

namespace {

class bridged_server : public rmi::server {
 public:
  bridged_server(rmi::transport::server_transport_iface& t, srv_app& app)
      : rmi::server(t), app_(app) {}

 protected:
  void on_session_created(rmi::context& ctx) override {
    // Call the app hook first so wish can initialise the session logger before
    // the trace fires.
    app_.on_session_created(ctx);
    std::ostringstream oss;
    oss << "[verbose] open        sid=0x"
        << std::hex << std::setw(8) << std::setfill('0') << ctx.session_id.id;
    on_print(ctx.session_id, oss.str());
  }

  void on_session_destroyed(rmi::context& ctx) override {
    std::ostringstream oss;
    oss << "[verbose] close       sid=0x"
        << std::hex << std::setw(8) << std::setfill('0') << ctx.session_id.id
        << " (" << std::dec << ctx.objects.size() << " objects)";
    on_print(ctx.session_id, oss.str());
    app_.on_session_destroyed(ctx);
  }

  void on_print(bison::key_t session_id, const std::string& line) override {
    if (FLAGS_verbose) app_.on_verbose_trace(session_id, line);
  }

  std::string on_help_text() const override {
    return app_.server_description();
  }

 private:
  srv_app& app_;
};

#if defined(__linux__)
/**
 * @brief Single-use transport adapter wrapping a pre-accepted PTY connection.
 *
 * `srv_app` accepts a connection from `pty_server_transport`, wraps it here,
 * and passes this adapter to a fresh `bridged_server`.  The adapter yields the
 * connection on the first `accept()` call and blocks on all subsequent calls
 * until `stop()` is invoked.
 */
class one_shot_transport final
    : public rmi::transport::server_transport_iface {
 public:
  explicit one_shot_transport(
      std::unique_ptr<rmi::transport::server_connection_iface> conn)
      : conn_(std::move(conn)) {}

  void start(bison::dynamic) override {}

  std::unique_ptr<rmi::transport::server_connection_iface> accept(
      std::chrono::milliseconds timeout) override {
    if (stopped_.load()) return nullptr;
    if (used_.exchange(true)) {
      std::unique_lock<std::mutex> lk(mtx_);
      cv_.wait_for(lk, timeout, [this] { return stopped_.load(); });
      return nullptr;
    }
    return std::move(conn_);
  }

  void stop() override {
    stopped_.store(true);
    if (conn_) conn_->close();
    cv_.notify_all();
  }

 private:
  std::unique_ptr<rmi::transport::server_connection_iface> conn_;
  std::atomic<bool>       used_{false};
  std::atomic<bool>       stopped_{false};
  std::mutex              mtx_;
  std::condition_variable cv_;
};
#endif // defined(__linux__)

} // namespace

// ── Default hook implementations ──────────────────────────────────────────────

void srv_app::on_verbose_trace(bison::key_t /*session_id*/,
                               const std::string& line) const {
  std::cout << line << '\n';
}

void srv_app::on_listening() const {
  if (!FLAGS_pipe.empty()) {
    std::cout << "[srv_app] listening on pipe " << FLAGS_pipe
              << " -- press Enter to stop\n" << std::flush;
  } else {
    std::cout << "[srv_app] listening on " << FLAGS_host << ':' << FLAGS_port
              << " -- press Enter to stop\n" << std::flush;
  }
}

void srv_app::on_session_created(rmi::context& ctx) const { (void)ctx; }

void srv_app::on_session_destroyed(rmi::context& ctx) const { (void)ctx; }

void srv_app::on_error(const std::string& msg) const {
  std::cerr << "[srv_app] error: " << msg << '\n';
}

#if defined(__linux__)
void srv_app::on_listening_pty() const {
  std::cout << "[srv_app] PTY server started -- waiting for connections\n"
            << std::flush;
}
#endif

// ── Default run_with_transport — socket/stream sessions ──────────────────────

int srv_app::run_with_transport(
    rmi::transport::server_transport_iface& transport) {
  bridged_server srv{transport, *this};
  srv.listen();
  on_listening();
  std::string line;
  std::getline(std::cin, line);
  srv.stop();
  return 0;
}

// ── Default run_pty — PTY lifecycle (Linux only) ──────────────────────────────

#if defined(__linux__)
int srv_app::run_pty() {
  pty_server_transport transport{"bash"};
  bison::dynamic pty_params;
  pty_params["mode"_key] = std::string{"dcs"};
  transport.start(std::move(pty_params));

  on_listening_pty();

  while (transport.is_shell_running()) {
    auto conn = transport.accept(std::chrono::milliseconds{200});
    if (!conn) {
      if (!transport.is_shell_running()) break;
      continue;
    }

    {
      one_shot_transport adapter{std::move(conn)};
      bridged_server srv{adapter, *this};
      srv.listen();

      while (!transport.wait_until_closed(std::chrono::milliseconds{200})) {
        if (!transport.is_shell_running()) break;
      }
    }

    if (!transport.is_shell_running()) break;
    transport.restart_session();
  }

  transport.stop();
  return 0;
}
#endif

// ── run() — argument parsing and lifecycle ────────────────────────────────────

int srv_app::run(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  try {
    register_classes();

#if defined(__linux__)
    if (FLAGS_pty) return run_pty();
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
