// MIT License © 2025 Binary Dice Games
/**
 * @file standalone_app.cpp
 * @brief In-process (transport-free) application scaffold implementation.
 */
#include "src/app/standalone/standalone_app.hpp"

#include "src/app/debugger.hpp"

#include <gflags/gflags.h>

#include <iostream>
#include <stdexcept>
#include <string>

DECLARE_bool(debugger);

namespace bdg::bison::app {

namespace {

/** Forwards standalone's session hooks to the app-level hooks. */
class bridged_standalone : public rmi::standalone {
 public:
  explicit bridged_standalone(standalone_app& app) : app_(app) {}

 protected:
  void on_session_created(rmi::context& ctx) override {
    app_.on_session_created(ctx);
  }

  void on_session_destroyed(rmi::context& ctx) override {
    app_.on_session_destroyed(ctx);
  }

 private:
  standalone_app& app_;
};

} // namespace

// ── Default hook implementations ──────────────────────────────────────────────

void standalone_app::on_error(const std::string& msg) const {
  std::cerr << "[standalone_app] error: " << msg << '\n';
}

std::unique_ptr<rmi::standalone> standalone_app::make_standalone() {
  return std::make_unique<bridged_standalone>(*this);
}

// ── run() — flag parsing and lifecycle ────────────────────────────────────────

int standalone_app::run(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_debugger) {
    wait_for_debugger();
  }

  try {
    register_classes();

    std::unique_ptr<rmi::standalone> sa = make_standalone();
    open_session(*sa);
    const int result = on_session(*sa);
    close_session(*sa);
    return result;
  } catch (const std::exception& ex) {
    on_error(ex.what());
    return 1;
  } catch (...) {
    on_error("unexpected failure");
    return 1;
  }
}

} // namespace bdg::bison::app
