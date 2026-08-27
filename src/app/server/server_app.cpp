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
#include "src/rmi/transport/tls_socket_transport.hpp"
#include "src/term/terminal.hpp"

#include <gflags/gflags.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

DECLARE_string(host);
DECLARE_int32(port);
DECLARE_string(name);
DECLARE_string(cmd);
DECLARE_bool(verbose);
DECLARE_bool(debugger);
DECLARE_string(cert_file);
DECLARE_string(cert_pem);
DECLARE_string(key_file);
DECLARE_string(key_pem);
DECLARE_string(key_password);
DECLARE_string(client_auth);
DECLARE_string(ca_file);
DECLARE_string(ca_pem);

namespace bdg::bison::app {

// ── Internal server subclass that bridges rmi hooks to server_app hooks ───────

namespace {

// Set from a signal handler, so it must only ever be touched with an atomic
// store/load (no locks, no allocation, no I/O -- the handler itself must
// stay async-signal-safe). Checked by wait_for_shutdown()/
// is_shutdown_requested() so Ctrl+C / SIGTERM reaches server::stop() (which
// closes every connected client's session/connection) instead of falling
// through to the OS's default action of killing the process outright.
std::atomic<bool> g_shutdown_requested{false};

extern "C" void handle_shutdown_signal(int /*sig*/) {
  g_shutdown_requested.store(true, std::memory_order_relaxed);
}

// Installing twice (e.g. run() called more than once in a process) is
// harmless -- std::signal() just overwrites the handler with itself -- so
// no once-guard is needed.
void install_shutdown_signal_handlers() {
  std::signal(SIGINT, handle_shutdown_signal);
  std::signal(SIGTERM, handle_shutdown_signal);
}

class bridged_server : public rmi::server {
 public:
  bridged_server(rmi::transport::server_transport_iface& t, server_app& app) : rmi::server(t), app_(app) {
    // `--verbose` is this app's "give me full detail" switch: it already
    // gates whether trace lines are printed at all (see on_print below), so
    // let it also opt into the decoded payloads that are off by default.
    set_trace_payloads(FLAGS_verbose);
  }

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
    case transport_kind::tls:
      std::cout << "[server_app] listening on " << FLAGS_host << ':' << FLAGS_port
                << " (tls) -- press Enter to stop\n"
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

// ── make_server — default server construction ─────────────────────────────────

std::unique_ptr<rmi::server> server_app::make_server(rmi::transport::server_transport_iface& transport) {
  return std::make_unique<bridged_server>(transport, *this);
}

void server_app::on_listen_params(bison::dynamic& params) const {
  if (selected_transport() != transport_kind::tls)
    return;
  params["cert_file"_key] = FLAGS_cert_file;
  params["cert_pem"_key] = FLAGS_cert_pem;
  params["key_file"_key] = FLAGS_key_file;
  params["key_pem"_key] = FLAGS_key_pem;
  params["key_password"_key] = FLAGS_key_password;
  params["client_auth"_key] = FLAGS_client_auth;
  params["ca_file"_key] = FLAGS_ca_file;
  params["ca_pem"_key] = FLAGS_ca_pem;
}

// ── Default run_with_transport — socket/stream sessions ──────────────────────

int server_app::run_with_transport(rmi::transport::server_transport_iface& transport) {
  std::unique_ptr<rmi::server> srv = make_server(transport);
  bison::dynamic params;
  on_listen_params(params);
  srv->listen(std::move(params));
  on_listening();
  wait_for_shutdown();
  srv->stop();
  return 0;
}

void server_app::wait_for_shutdown() {
  if (active_term_) {
    while (!active_term_->has_exited() && !g_shutdown_requested.load(std::memory_order_relaxed))
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    return;
  }

  // Blocking on stdin has no portable way to be interrupted from another
  // thread, so read it on a helper thread and poll the shutdown flag here
  // instead of blocking on it directly -- that's what lets Ctrl+C/SIGTERM
  // stop the server even while nothing has been typed yet. This
  // deliberately uses a raw read(2)/_read() on the fd rather than
  // std::getline(std::cin, ...): on the signal path, nothing is ever typed,
  // so this thread is left detached and permanently blocked here -- a raw
  // fd read has no C++ runtime global object for that to be unsafe with,
  // whereas a thread still inside std::cin's machinery when process exit
  // destroys std::cin's static state is a data race (observed as a hang:
  // the reader was scheduled back in mid-teardown and never returned).
  // Process exit simply discards a thread blocked in a raw syscall.
  auto got_line = std::make_shared<std::atomic<bool>>(false);
  std::thread reader([got_line] {
    char buf[256];
#if defined(_WIN32)
    [[maybe_unused]] auto n = _read(0, buf, sizeof(buf));
#else
    [[maybe_unused]] auto n = read(STDIN_FILENO, buf, sizeof(buf));
#endif
    got_line->store(true, std::memory_order_relaxed);
  });
  reader.detach();

  while (!got_line->load(std::memory_order_relaxed) && !g_shutdown_requested.load(std::memory_order_relaxed))
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
}

bool server_app::is_shutdown_requested() const {
  return (active_term_ && active_term_->has_exited()) || g_shutdown_requested.load(std::memory_order_relaxed);
}

// ── run() — argument parsing and lifecycle ────────────────────────────────────

int server_app::run(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  install_shutdown_signal_handlers();

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
      case transport_kind::tls: {
        auto port = static_cast<uint16_t>(FLAGS_port);
        rmi::transport::tls_socket_server_transport tls_transport{FLAGS_host, port};
        return run_with_transport(tls_transport);
      }
      case transport_kind::term: {
        term::terminal term_proc{FLAGS_cmd, terminal_label()};
        term_proc.start_pump();
        rmi::transport::term_server_transport term_transport{term_proc.read_handle(), term_proc.write_handle()};
        active_term_ = &term_proc;
        int rc = run_with_transport(term_transport);
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
