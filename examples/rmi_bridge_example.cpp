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

DEFINE_string(downstream_transport, "tcp", "Downstream transport to use: tcp, pipe, tls, or term");
DEFINE_string(downstream_host, "0.0.0.0", "Downstream bind host address (downstream_transport=tcp/tls)");
DEFINE_int32(downstream_port, 7071, "Downstream listen port (downstream_transport=tcp/tls)");
DEFINE_string(downstream_name, "", "Downstream named-pipe / Unix-socket path (downstream_transport=pipe)");
DEFINE_string(cmd, "", "Command to spawn (downstream_transport=term)");
DEFINE_string(downstream_cert_file, "", "Downstream server certificate chain file (downstream_transport=tls)");
DEFINE_string(downstream_cert_pem, "", "Downstream server certificate chain PEM (downstream_transport=tls)");
DEFINE_string(downstream_key_file, "", "Downstream server private key file (downstream_transport=tls)");
DEFINE_string(downstream_key_pem, "", "Downstream server private key PEM (downstream_transport=tls)");
DEFINE_string(downstream_key_password, "", "Passphrase for an encrypted downstream private key (downstream_transport=tls)");
DEFINE_string(downstream_client_auth, "none",
              "Downstream mutual TLS mode: none, optional, or required (downstream_transport=tls)");
DEFINE_string(downstream_ca_file, "",
              "Trust anchor file for verifying downstream client certs (downstream_transport=tls, client_auth!=none)");
DEFINE_string(downstream_ca_pem, "",
              "Trust anchor PEM for verifying downstream client certs (downstream_transport=tls, client_auth!=none)");

DEFINE_string(upstream_transport, "term", "Upstream transport to use: tcp, pipe, tls, or term");
DEFINE_string(upstream_host, "127.0.0.1", "Upstream host address (upstream_transport=tcp/tls)");
DEFINE_int32(upstream_port, 7070, "Upstream port (upstream_transport=tcp/tls)");
DEFINE_string(upstream_name, "", "Upstream named-pipe / Unix-socket path (upstream_transport=pipe)");
DEFINE_string(upstream_ca_file, "", "Trust anchor file for verifying the upstream server's cert (upstream_transport=tls)");
DEFINE_string(upstream_ca_pem, "", "Trust anchor PEM for verifying the upstream server's cert (upstream_transport=tls)");
DEFINE_string(upstream_server_name, "", "SNI / hostname-verification target (upstream_transport=tls, default: --upstream_host)");
DEFINE_bool(upstream_insecure_skip_verify, false,
            "Skip upstream server certificate verification -- unsafe, dev/test only (upstream_transport=tls)");
DEFINE_string(upstream_cert_file, "", "Upstream client certificate file, for mutual TLS (upstream_transport=tls)");
DEFINE_string(upstream_cert_pem, "", "Upstream client certificate PEM, for mutual TLS (upstream_transport=tls)");
DEFINE_string(upstream_key_file, "", "Upstream client private key file, for mutual TLS (upstream_transport=tls)");
DEFINE_string(upstream_key_pem, "", "Upstream client private key PEM, for mutual TLS (upstream_transport=tls)");
DEFINE_string(upstream_key_password, "", "Passphrase for an encrypted upstream private key (upstream_transport=tls)");

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
