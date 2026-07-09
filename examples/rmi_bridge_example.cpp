// MIT License © 2025 Binary Dice Games
// examples/rmi_bridge_example.cpp
//
// Standalone RMI bridge example built on the bridge_app scaffold. A bridge
// accepts downstream client connections and transparently relays every
// operation to one upstream server -- run alongside rmi_server_example and
// rmi_client_example to see a client talk through the bridge exactly as if
// it were talking directly to the server. See docs/examples.md for the full
// three-process walkthrough.

#include "src/app/bridge/bridge_app.hpp"
#include "src/bison/bison_flags.hpp"

#include <gflags/gflags.h>

DEFINE_string(transport, "tcp", "Downstream transport to use: tcp, pipe, or term");
DEFINE_string(host, "0.0.0.0", "Downstream bind host address (transport=tcp)");
DEFINE_int32(port, 7071, "Downstream listen port (transport=tcp)");
DEFINE_string(name, "", "Downstream named-pipe / Unix-socket path (transport=pipe)");
DEFINE_string(cmd, "", "Command to spawn (transport=term)");

DEFINE_string(upstream_transport, "term", "Upstream transport to use: tcp, pipe, or term");
DEFINE_string(upstream_host, "127.0.0.1", "Upstream host address (upstream_transport=tcp)");
DEFINE_int32(upstream_port, 7070, "Upstream port (upstream_transport=tcp)");
DEFINE_string(upstream_name, "", "Upstream named-pipe / Unix-socket path (upstream_transport=pipe)");

DEFINE_int32(timeout, 30000, "Upstream per-request timeout in milliseconds");
DEFINE_bool(verbose, false, "Print downstream request/response trace messages to stdout");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");

namespace {

/** @brief Plain relay: no local class registry, bridge_app's defaults do everything. */
class relay_bridge_app : public bdg::bison::app::bridge_app {
 protected:
  std::string bridge_description() const override {
    return "Standalone RMI bridge example -- relays downstream clients to one upstream server.";
  }
};

} // namespace

int main(int argc, char** argv) {
  if (bdg::bison::print_usage(argc, argv, "Standalone RMI bridge example.", __FILE__))
    return 0;

  relay_bridge_app app;
  return app.run(argc, argv);
}
