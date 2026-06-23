// MIT License © 2025 Binary Dice Games
/**
 * @file pty_server_app.cpp
 * @brief PTY server application scaffold — delegates to srv_app::run_pty().
 */
#include "src/app/pty/pty_server_app.hpp"

#if defined(__linux__)

namespace bdg::bison::app {

int pty_server_app::run(int argc, char** argv) {
  (void)argc;
  (void)argv;
  try {
    register_classes();
    return run_pty();
  } catch (const std::exception& ex) {
    on_error(ex.what());
    return 1;
  } catch (...) {
    on_error("unexpected failure");
    return 1;
  }
}

bison::dynamic pty_server_app::listen_params() const {
  bison::dynamic params;
  params["mode"_key] = std::string{"dcs"};
  return params;
}

} // namespace bdg::bison::app

#endif // defined(__linux__)
