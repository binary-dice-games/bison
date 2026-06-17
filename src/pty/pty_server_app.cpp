// MIT License © 2025 Binary Dice Games
/**
 * @file pty_server_app.cpp
 * @brief Multi-session PTY server application scaffold implementation.
 */
#include "src/pty/pty_server_app.hpp"

#if defined(__linux__)

#include "src/pty/pty_server_transport.hpp"
#include "src/rmi/server/server.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <chrono>
#include <iostream>
#include <thread>

namespace bdg::bison::pty {

namespace {

/**
 * @brief Single-use `server_transport_iface` that wraps a pre-accepted connection.
 *
 * `pty_server_app` calls `pty_server_transport::accept()` to detect a client,
 * then wraps the returned connection in this adapter so `rmi::server` can be
 * constructed normally.  The adapter hands the connection to the server's
 * accept-loop on the first call and blocks on all subsequent calls until
 * `stop()` is invoked.
 */
class one_shot_transport final : public rmi::transport::server_transport_iface {
 public:
  explicit one_shot_transport(
      std::unique_ptr<rmi::transport::server_connection_iface> conn)
      : conn_(std::move(conn)) {}

  /** @brief No-op — transport params were applied when the PTY was started. */
  void start(bison::dynamic) override {}

  /**
   * @brief Return the pre-accepted connection on the first call; block thereafter.
   *
   * Subsequent calls block for up to @p timeout waiting for `stop()`.  This
   * keeps the `rmi::server` accept thread alive without busy-waiting.
   */
  std::unique_ptr<rmi::transport::server_connection_iface> accept(
      std::chrono::milliseconds timeout) override {
    if (stopped_.load())
      return nullptr;
    if (used_.exchange(true)) {
      std::unique_lock<std::mutex> lk(mtx_);
      cv_.wait_for(lk, timeout, [this] { return stopped_.load(); });
      return nullptr;
    }
    return std::move(conn_);
  }

  /** @brief Signal the accept loop to exit and close the connection. */
  void stop() override {
    stopped_.store(true);
    if (conn_)
      conn_->close();
    cv_.notify_all();
  }

 private:
  std::unique_ptr<rmi::transport::server_connection_iface> conn_;
  std::atomic<bool>       used_{false};
  std::atomic<bool>       stopped_{false};
  std::mutex              mtx_;
  std::condition_variable cv_;
};

} // namespace

// ── Default hook implementations ──────────────────────────────────────────

std::string pty_server_app::shell_command() const {
  return "bash";
}

bison::dynamic pty_server_app::listen_params() const {
  bison::dynamic p;
  p["mode"_key] = std::string{"dcs"};
  return p;
}

void pty_server_app::on_client_connected() const {}
void pty_server_app::on_session_ended() const {}

void pty_server_app::on_error(const std::string& msg) const {
  std::cerr << "[pty_server_app] error: " << msg << '\n';
}

// ── run() ─────────────────────────────────────────────────────────────────

int pty_server_app::run(int argc, char** argv) {
  (void)argc;
  (void)argv;

  try {
    register_classes();

    pty_server_transport transport{shell_command()};
    transport.start(listen_params());

    while (transport.is_shell_running()) {
      // Block until the bison client sends a HELLO through the PTY channel.
      auto conn = transport.accept(std::chrono::milliseconds{200});
      if (!conn) {
        if (!transport.is_shell_running())
          break;
        continue;
      }

      on_client_connected();

      // Serve the session via a fresh rmi::server so all session objects are
      // destroyed when the server goes out of scope.
      {
        rmi::server srv{
            std::make_unique<one_shot_transport>(std::move(conn))};
        srv.listen();

        // Wait for the session to end (END frame, connection drop, or shell exit).
        while (!transport.wait_until_closed(std::chrono::milliseconds{200})) {
          if (!transport.is_shell_running())
            break;
        }

        // Destroy srv: stops accept thread, joins workers, destroys all
        // session objects via their context destructors.
      }

      on_session_ended();

      if (!transport.is_shell_running())
        break;

      transport.restart_session();
    }

    transport.stop();
    return 0;
  } catch (const std::exception& ex) {
    on_error(ex.what());
    return 1;
  } catch (...) {
    on_error("unexpected failure");
    return 1;
  }
}

} // namespace bdg::bison::pty

#endif // defined(__linux__)
