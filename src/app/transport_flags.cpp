// MIT License © 2025 Binary Dice Games
/**
 * @file transport_flags.cpp
 * @brief Shared `--transport` flag handling for `server_app` / `client_app`.
 */
#include "src/app/transport_flags.hpp"

#include <gflags/gflags.h>

#include <stdexcept>
#include <string>

DECLARE_string(transport);

namespace bdg::bison::app {

transport_kind selected_transport() {
  if (FLAGS_transport == "tcp")
    return transport_kind::tcp;
  if (FLAGS_transport == "pipe")
    return transport_kind::pipe;
  if (FLAGS_transport == "pty")
    return transport_kind::pty;
  throw std::runtime_error("invalid --transport value '" + FLAGS_transport + "' (expected tcp, pipe, or pty)");
}

} // namespace bdg::bison::app
