// MIT License © 2025 Binary Dice Games
/**
 * @file pty_c.cpp
 * @brief C++ implementation of the pure-C PTY application scaffold API.
 *
 * Wraps `pty_client_app` and `pty_server_app` in callback-driven subclasses
 * so that C callers can participate in every virtual hook without linking C++.
 * All C++ exceptions are caught at the C boundary and converted to `rmi_error`.
 */

#include "include/pty_c.h"
#include "src/rmi/rmi.hpp"

#if defined(__linux__)
#include "src/app/pty/pty_client_app.hpp"
#include "src/app/pty/pty_server_app.hpp"
#endif

#include <stdexcept>
#include <string>

using namespace bdg::bison;
using namespace bdg::bison::rmi;

// ─── Internal helpers ──────────────────────────────────────────────────────

using bison_dynamic_ptr = std::shared_ptr<dynamic>;

struct client_state {
  std::unique_ptr<client> client_owner;
  std::unique_ptr<standalone> standalone_owner;
  client* borrowed_client = nullptr;
  bool owns_resource = true;
  bool released = false;

  bool is_standalone() const { return standalone_owner != nullptr; }

  bool is_valid() const {
    if (released)
      return false;
    if (is_standalone())
      return standalone_owner != nullptr;
    return (owns_resource ? client_owner.get() : borrowed_client) != nullptr;
  }
};

static inline client_state* as_client_state(rmi_client_handle h) {
  return reinterpret_cast<client_state*>(h);
}

static inline rmi_client_handle as_client_handle(client_state* p) {
  return reinterpret_cast<rmi_client_handle>(p);
}

static inline bison_dynamic_ptr* as_dynamic_ptr(bison_handle h) {
  return reinterpret_cast<bison_dynamic_ptr*>(h);
}

static inline bison_handle as_bison_handle(bison_dynamic_ptr* p) {
  return reinterpret_cast<bison_handle>(p);
}

static inline dynamic bison_handle_to_dynamic(bison_handle h) {
  if (!h)
    return dynamic{};
  bison_dynamic_ptr* p = as_dynamic_ptr(h);
  if (!*p)
    return dynamic{};
  return dynamic(**p);
}

static inline rmi_client_handle make_borrowed_client_handle(client& borrowed) {
  auto* state = new client_state{};
  state->borrowed_client = &borrowed;
  state->owns_resource = false;
  state->released = false;
  return as_client_handle(state);
}

static inline rmi_error map_runtime_error(const std::runtime_error& e) {
  std::string msg = e.what();
  if (msg.find("remote") != std::string::npos ||
      msg.find("server") != std::string::npos)
    return RMI_ERR_REMOTE_EXCEPTION;
  return RMI_ERR_INVALID_STATE;
}

static inline rmi_error map_app_exit_code(int exit_code) {
  return exit_code == 0 ? RMI_OK : RMI_ERR_INVALID_STATE;
}

// ─────────────────────────────────────────────────────────────────────────────

#if defined(__linux__)

// ── Client wrapper ────────────────────────────────────────────────────────────

class c_pty_client_application_with_callbacks final
    : public app::pty_client_app {
 public:
  explicit c_pty_client_application_with_callbacks(
      const pty_client_callbacks* callbacks)
      : callbacks_(callbacks) {}

 protected:
  int on_session(client& rmi_client) override {
    rmi_client_handle callback_client = make_borrowed_client_handle(rmi_client);
    int result = 1;
    try {
      result = callbacks_->on_session(callback_client, callbacks_->user);
    } catch (...) {
      result = 1;
    }
    delete as_client_state(callback_client);
    return result;
  }

  void on_connected() const override {
    if (callbacks_ && callbacks_->on_connected)
      callbacks_->on_connected(callbacks_->user);
  }

  void on_error(const std::string& message) const override {
    if (callbacks_ && callbacks_->on_error) {
      callbacks_->on_error(message.c_str(), callbacks_->user);
      return;
    }
    app::pty_client_app::on_error(message);
  }

  void on_connect_params(dynamic& params) const override {
    if (!callbacks_ || !callbacks_->on_connect_params) {
      app::pty_client_app::on_connect_params(params);
      return;
    }
    // Wrap params in a temporary bison_handle so the C callback can modify it
    // via bison_set_* APIs, then copy the result back.
    bison_dynamic_ptr holder = std::make_shared<dynamic>(params);
    bison_handle h = as_bison_handle(&holder);
    callbacks_->on_connect_params(h, callbacks_->user);
    params = std::move(*holder);
  }

 private:
  const pty_client_callbacks* callbacks_ = nullptr;
};

// ── Server wrapper ────────────────────────────────────────────────────────────

class c_pty_server_application_with_callbacks final
    : public app::pty_server_app {
 public:
  explicit c_pty_server_application_with_callbacks(
      const pty_server_callbacks* callbacks)
      : callbacks_(callbacks) {}

 protected:
  void register_classes() override {
    callbacks_->register_classes(callbacks_->user);
  }

  std::string shell_command() const override {
    if (!callbacks_ || !callbacks_->shell_command)
      return app::pty_server_app::shell_command();
    const char* cmd = callbacks_->shell_command(callbacks_->user);
    return cmd ? std::string{cmd} : app::pty_server_app::shell_command();
  }

  dynamic listen_params() const override {
    if (!callbacks_ || !callbacks_->listen_params)
      return app::pty_server_app::listen_params();
    bison_handle h = callbacks_->listen_params(callbacks_->user);
    if (!h)
      return dynamic{};
    dynamic result = bison_handle_to_dynamic(h);
    delete as_dynamic_ptr(h);
    return result;
  }

  void on_client_connected() const override {
    if (callbacks_ && callbacks_->on_client_connected)
      callbacks_->on_client_connected(callbacks_->user);
  }

  void on_session_ended() const override {
    if (callbacks_ && callbacks_->on_session_ended)
      callbacks_->on_session_ended(callbacks_->user);
  }

  void on_error(const std::string& message) const override {
    if (callbacks_ && callbacks_->on_error) {
      callbacks_->on_error(message.c_str(), callbacks_->user);
      return;
    }
    app::pty_server_app::on_error(message);
  }

 private:
  const pty_server_callbacks* callbacks_ = nullptr;
};

#endif // defined(__linux__)

// ─── C API ───────────────────────────────────────────────────────────────────

RMI_API rmi_error pty_client_run(
    int argc,
    char** argv,
    const pty_client_callbacks* callbacks) {
  if (argc > 0 && !argv)
    return RMI_ERR_NULL;
  if (!callbacks || !callbacks->on_session)
    return RMI_ERR_NULL;
#if defined(__linux__)
  try {
    c_pty_client_application_with_callbacks app(callbacks);
    return map_app_exit_code(app.run(argc, argv));
  } catch (const std::runtime_error& e) {
    return map_runtime_error(e);
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
#else
  (void)argc;
  (void)argv;
  return RMI_ERR_INVALID_STATE;
#endif
}

RMI_API rmi_error pty_server_run(
    int argc,
    char** argv,
    const pty_server_callbacks* callbacks) {
  if (argc > 0 && !argv)
    return RMI_ERR_NULL;
  if (!callbacks || !callbacks->register_classes)
    return RMI_ERR_NULL;
#if defined(__linux__)
  try {
    c_pty_server_application_with_callbacks app(callbacks);
    return map_app_exit_code(app.run(argc, argv));
  } catch (const std::runtime_error& e) {
    return map_runtime_error(e);
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
#else
  (void)argc;
  (void)argv;
  return RMI_ERR_INVALID_STATE;
#endif
}
