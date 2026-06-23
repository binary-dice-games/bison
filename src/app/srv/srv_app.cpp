// MIT License © 2025 Binary Dice Games
/**
 * @file srv_app.cpp
 * @brief Extensible server application scaffold implementation.
 */
#include "src/app/srv/srv_app.hpp"

#include "src/bison/bison_print.hpp"
#include "src/rmi/server/server.hpp"
#include "src/rmi/shared/constants.hpp"
#include "src/rmi/shared/envelope.hpp"
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

#include <cstdint>
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

// ── Verbose trace helpers ─────────────────────────────────────────────────────

static const char* op_to_label(bison::key_t op) {
  using namespace rmi::shared::constants;
  if (op == OP_CONNECT)     return "connect    ";
  if (op == OP_DISCONNECT)  return "disconnect ";
  if (op == OP_INSTANTIATE) return "instantiate";
  if (op == OP_CALL)        return "call       ";
  if (op == OP_GET)         return "get        ";
  if (op == OP_SET)         return "set        ";
  if (op == OP_DESTROY)     return "destroy    ";
  if (op == OP_CLEAR)       return "clear      ";
  if (op == OP_DESCRIBE)    return "describe   ";
  if (op == OP_DICTIONARY)  return "dictionary ";
  if (op == OP_HELP)        return "help       ";
  return "unknown    ";
}

class bridged_server : public rmi::server {
 public:
  bridged_server(rmi::transport::server_transport_iface& t, srv_app& app)
      : rmi::server(t), app_(app) {}

 protected:
  void on_session_created(rmi::context& ctx) override {
    std::call_once(dict_flag_, [this] {
      dict_ = bison::build_display_dict();
    });
    // Call the app hook first so wish can initialise the session logger before
    // the trace fires.
    app_.on_session_created(ctx);
    if (FLAGS_verbose) {
      std::ostringstream oss;
      oss << "[verbose] open        sid=0x"
          << std::hex << std::setw(8) << std::setfill('0') << ctx.session_id.id;
      app_.on_verbose_trace(ctx.session_id, oss.str());
    }
  }

  void on_session_destroyed(rmi::context& ctx) override {
    if (FLAGS_verbose) {
      std::ostringstream oss;
      oss << "[verbose] close       sid=0x"
          << std::hex << std::setw(8) << std::setfill('0') << ctx.session_id.id
          << " (" << std::dec << ctx.objects.size() << " objects)";
      app_.on_verbose_trace(ctx.session_id, oss.str());
    }
    app_.on_session_destroyed(ctx);
  }

  void on_request_trace(
      rmi::context& ctx,
      const rmi::shared::envelope& env) override {
    if (!FLAGS_verbose) return;
    using namespace rmi::shared::constants;

    bison::print_options popts;
    popts.multiline = false;
    popts.dict = &dict_;

    std::ostringstream oss;
    oss << "[verbose] " << op_to_label(env.op)
        << " sid=0x" << std::hex << std::setw(8) << std::setfill('0')
        << ctx.session_id.id;

    const bison::key_t op = env.op;
    if (op == OP_INSTANTIATE || op == OP_DESCRIBE) {
      const auto* f = env.payload.findField(FIELD_KLASS);
      if (f && f->is<bison::key_t>()) {
        const auto h = f->as<bison::key_t>().id;
        auto it = dict_.find(h);
        if (it != dict_.end()) {
          oss << " class=" << it->second;
        } else {
          oss << " class=#" << std::hex << std::setw(8) << std::setfill('0') << h;
        }
      }
    } else if (op == OP_CALL) {
      oss << std::dec << " obj=0x" << std::hex << std::setw(8)
          << std::setfill('0') << env.object_id.id;
      const auto* f = env.payload.findField(FIELD_NAME);
      if (f && f->is<bison::key_t>()) {
        const auto h = f->as<bison::key_t>().id;
        auto it = dict_.find(h);
        if (it != dict_.end()) {
          oss << " method=" << it->second;
        } else {
          oss << " method=#" << std::hex << std::setw(8) << std::setfill('0') << h;
        }
      }
      // Print call arguments.
      const auto* pf = env.payload.findField(FIELD_PARAMS);
      if (pf && pf->is<bison::dynamic_ptr>()) {
        auto ptr = pf->as<bison::dynamic_ptr>();
        if (ptr && !ptr->empty())
          oss << " args=" << bison::print(*ptr, popts);
      }
    } else if (op == OP_SET) {
      oss << std::dec << " obj=0x" << std::hex << std::setw(8)
          << std::setfill('0') << env.object_id.id;
      if (!env.payload.empty())
        oss << " " << bison::print(env.payload, popts);
    } else if (op == OP_GET || op == OP_DESTROY || op == OP_CLEAR) {
      oss << std::dec << " obj=0x" << std::hex << std::setw(8)
          << std::setfill('0') << env.object_id.id;
    }

    app_.on_verbose_trace(ctx.session_id, oss.str());
  }

  void on_response_trace(
      rmi::context& ctx,
      const rmi::shared::envelope& request_env,
      bison::key_t op,
      bool is_error,
      bison::key_t error_code,
      const bison::dynamic& response_payload) override {
    if (!FLAGS_verbose) return;
    using namespace rmi::shared::constants;

    bison::print_options popts;
    popts.multiline = false;
    popts.dict = &dict_;

    std::ostringstream oss;
    oss << "[verbose] " << op_to_label(op)
        << (is_error ? " ERROR" : " ok   ")
        << " sid=0x" << std::hex << std::setw(8) << std::setfill('0')
        << ctx.session_id.id;

    if (is_error) {
      oss << " code=0x" << error_code.id;
    } else if (!response_payload.empty()) {
      // For instantiate/describe, label the class; for others, just dump.
      if (op == OP_INSTANTIATE) {
        const auto* kf = response_payload.findField(FIELD_KLASS);
        if (kf && kf->is<bison::key_t>()) {
          const auto h = kf->as<bison::key_t>().id;
          auto it = dict_.find(h);
          if (it != dict_.end()) {
            oss << " class=" << it->second;
          } else {
            oss << " class=#" << std::hex << std::setw(8) << std::setfill('0') << h;
          }
        }
        const auto* of = response_payload.findField(FIELD_OBJECT_ID);
        if (of && of->is<bison::key_t>())
          oss << std::dec << " obj=0x" << std::hex << std::setw(8)
              << std::setfill('0') << of->as<bison::key_t>().id;
      } else if (op == OP_CALL || op == OP_GET) {
        oss << std::dec << " obj=0x" << std::hex << std::setw(8)
            << std::setfill('0') << request_env.object_id.id;
        if (!response_payload.empty())
          oss << " " << bison::print(response_payload, popts);
      }
    }

    app_.on_verbose_trace(ctx.session_id, oss.str());
  }

  std::string on_help_text() const override {
    return app_.server_description();
  }

 private:
  srv_app& app_;
  std::once_flag dict_flag_;
  std::unordered_map<bison::hash_t, std::string> dict_;
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
