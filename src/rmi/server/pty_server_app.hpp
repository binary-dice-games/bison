// MIT License © 2025 Binary Dice Games
/**
 * @file pty_server_app.hpp
 * @brief Reusable PTY server application scaffold for RMI stdio flows.
 */
#pragma once

#include "src/rmi/server/server.hpp"

namespace bdg::bison::rmi::apps {

class pty_server_application {
 public:
  virtual ~pty_server_application() = default;

  int run(int argc, char** argv);

 protected:
  virtual void on_listen_params(bison::dynamic& params) const;
  virtual void register_classes() = 0;
  virtual void on_listening() const;
  virtual void on_waiting_for_disconnect() const;
  virtual void on_stopped() const;
  virtual void on_error(const std::string& message) const;
};

} // namespace bdg::bison::rmi::apps
