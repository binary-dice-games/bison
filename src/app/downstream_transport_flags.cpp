// MIT License © 2025 Binary Dice Games
/**
 * @file downstream_transport_flags.cpp
 * @brief `--downstream_transport` flag handling for `bridge_app`.
 *
 * Declared in transport_flags.hpp alongside `selected_transport()`, but kept
 * in its own translation unit for the same reason as
 * `upstream_transport_flags.cpp`: static-lib linking pulls in whole object
 * files, so keeping `selected_downstream_transport()` out of
 * transport_flags.cpp means binaries that only use `server_app`/`client_app`
 * (which never call it) aren't forced to also
 * `DEFINE_string(downstream_transport, ...)` just to satisfy the linker.
 */
#include "src/app/transport_flags.hpp"

#include <gflags/gflags.h>

#include <stdexcept>
#include <string>

DECLARE_string(downstream_transport);

namespace bdg::bison::app {

transport_kind selected_downstream_transport() {
  if (FLAGS_downstream_transport == "tcp")
    return transport_kind::tcp;
  if (FLAGS_downstream_transport == "pipe")
    return transport_kind::pipe;
  if (FLAGS_downstream_transport == "tls")
    return transport_kind::tls;
  if (FLAGS_downstream_transport == "term")
    return transport_kind::term;
  throw std::runtime_error(
      "invalid --downstream_transport value '" + FLAGS_downstream_transport +
      "' (expected tcp, pipe, tls, or term)");
}

} // namespace bdg::bison::app
