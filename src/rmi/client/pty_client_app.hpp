// MIT License © 2025 Binary Dice Games
/**
 * @file pty_client_app.hpp
 * @brief Reusable PTY/pipe client application scaffold for RMI stdio flows.
 */
#pragma once

#include "src/rmi/client/client.hpp"

#include <string>

namespace bdg::bison::rmi::apps {

class pty_client_application {
 public:
  virtual ~pty_client_application() = default;

  enum class launch_mode {
    pty,
    pipe,
  };

  struct run_context {
    launch_mode mode = launch_mode::pty;
    std::string command;
  };

  int run(int argc, char** argv);

 protected:
  virtual void on_usage() const;
  virtual void on_subprocess_started(const run_context& ctx) const;
  virtual void on_waiting_for_hello(const run_context& ctx) const;
  virtual void on_connected(const run_context& ctx) const;
  virtual void on_waiting_for_console_close(const run_context& ctx) const;
  virtual void on_connect_params(bison::dynamic& params, const run_context& ctx)
      const;
  virtual int on_session(client& rmi_client, const run_context& ctx);
  virtual void on_error(const std::string& message) const;
};

} // namespace bdg::bison::rmi::apps
