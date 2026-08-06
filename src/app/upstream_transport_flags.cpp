// MIT License © 2025 Binary Dice Games
/**
 * @file upstream_transport_flags.cpp
 * @brief `--upstream_transport` flag handling for `bridge_app`.
 *
 * Declared in transport_flags.hpp alongside `selected_transport()`, but kept
 * in its own translation unit: static-lib linking pulls in whole object
 * files, so keeping `selected_upstream_transport()` out of transport_flags.cpp
 * means binaries that only use `server_app`/`client_app` (which never call
 * it) aren't forced to also `DEFINE_string(upstream_transport, ...)` just to
 * satisfy the linker.
 */
#include "src/app/transport_flags.hpp"

#include <gflags/gflags.h>

#include <stdexcept>
#include <string>

DECLARE_string(upstream_transport);

namespace bdg::bison::app {

transport_kind selected_upstream_transport() {
  if (FLAGS_upstream_transport == "tcp")
    return transport_kind::tcp;
  if (FLAGS_upstream_transport == "pipe")
    return transport_kind::pipe;
  if (FLAGS_upstream_transport == "tls")
    return transport_kind::tls;
  if (FLAGS_upstream_transport == "term")
    return transport_kind::term;
  throw std::runtime_error(
      "invalid --upstream_transport value '" + FLAGS_upstream_transport + "' (expected tcp, pipe, tls, or term)");
}

} // namespace bdg::bison::app
