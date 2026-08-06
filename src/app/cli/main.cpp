// MIT License © 2025 Binary Dice Games
/**
 * @file main.cpp
 * @brief Entry point for the standalone bison-cli interactive REPL.
 */
#include "src/app/cli/cli_app.hpp"
#include "src/bison/bison_flags.hpp"

#include <gflags/gflags.h>

DEFINE_string(transport, "term", "Transport to use: tcp, pipe, tls, or term");
DEFINE_string(host, "127.0.0.1", "Server host address (transport=tcp/tls)");
DEFINE_int32(port, 7070, "Server TCP port (transport=tcp/tls)");
DEFINE_string(name, "", "Named-pipe / Unix-socket path (transport=pipe)");
DEFINE_int32(timeout, 30000, "Per-request timeout in milliseconds");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");
DEFINE_string(ca_file, "", "Trust anchor file for verifying the server's certificate (transport=tls)");
DEFINE_string(ca_pem, "", "Trust anchor PEM for verifying the server's certificate (transport=tls)");
DEFINE_string(server_name, "", "SNI / hostname-verification target (transport=tls, default: --host)");
DEFINE_bool(insecure_skip_verify, false, "Skip server certificate verification -- unsafe, dev/test only (transport=tls)");
DEFINE_string(cert_file, "", "Client certificate file, for mutual TLS (transport=tls)");
DEFINE_string(cert_pem, "", "Client certificate PEM, for mutual TLS (transport=tls)");
DEFINE_string(key_file, "", "Client private key file, for mutual TLS (transport=tls)");
DEFINE_string(key_pem, "", "Client private key PEM, for mutual TLS (transport=tls)");
DEFINE_string(key_password, "", "Passphrase for an encrypted client private key (transport=tls)");

int main(int argc, char** argv) {
  if (bdg::bison::print_usage(argc, argv, "Interactive REPL client for bison RMI servers.", __FILE__))
    return 0;

  bdg::bison::app::cli_app app;
  return app.run(argc, argv);
}
