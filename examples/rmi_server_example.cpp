// MIT License © 2025 Binary Dice Games
// examples/rmi_server_example.cpp
//
// Standalone RMI server example built directly on rmi::server (rather than
// the server_app scaffold used by calc-server), to demonstrate manual
// transport construction. Takes the same --transport/--host/--port/--name/
// --cmd/--debugger flags as calc-server (src/srv/calc/main.cpp) so usage is
// consistent across the project -- see docs/examples.md for per-transport
// walkthroughs.

#include "src/app/transport_flags.hpp"
#include "src/bison/bison_flags.hpp"
#include "src/pty/pty_write.hpp"
#include "src/rmi/rmi.hpp"
#include "src/rmi/transport/term_transport.hpp"
#include "src/term/terminal.hpp"

#include <gflags/gflags.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;
using namespace bdg::bison::rmi::shared::constants;

DEFINE_string(transport, "term", "Transport to use: tcp, pipe, or term");
DEFINE_string(host, "0.0.0.0", "Bind host address (transport=tcp)");
DEFINE_int32(port, 7070, "Listen port (transport=tcp)");
DEFINE_string(name, "", "Named-pipe / Unix-socket path (transport=pipe)");
DEFINE_string(cmd, "", "Command to spawn (transport=term)");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");

extern void wait_for_debugger();

static void register_calculator() {
  auto proto = dynamic_ptr{"Calculator"_key, {}};

  proto->addMethod("add"_key, method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
                     float a = params["a"_key];
                     float b = params["b"_key];
                     dynamic result;
                     result["result"_key] = a + b;
                     return result;
                   }});

  proto->addMethod("subtract"_key, method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
                     float a = params["a"_key];
                     float b = params["b"_key];
                     dynamic result;
                     result["result"_key] = a - b;
                     return result;
                   }});

  proto->addMethod("multiply"_key, method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
                     float a = params["a"_key];
                     float b = params["b"_key];
                     dynamic result;
                     result["result"_key] = a * b;
                     return result;
                   }});

  proto->addMethod("divide"_key, method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
                     float a = params["a"_key];
                     float b = params["b"_key];
                     dynamic result;
                     if (b == 0.0f) {
                       result["error"_key] = std::string{"division by zero"};
                       result["result"_key] = 0.0f;
                     } else {
                       result["result"_key] = a / b;
                     }
                     return result;
                   }});

  dynamic::addClass(0U, proto, 0U);
}

int main(int argc, char** argv) {
  if (bdg::bison::print_usage(argc, argv, "Standalone RMI Calculator server example.", __FILE__))
    return 0;

  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_debugger) {
    wait_for_debugger();
  }

  register_calculator();

  try {
    switch (app::selected_transport()) {
      case app::transport_kind::term: {
        term::terminal term_proc{FLAGS_cmd};
        term_proc.start_pump();
        term_server_transport transport{term_proc.read_handle(), term_proc.write_handle()};
        server srv{transport};
        srv.listen();

        // Same raw-mode/CRLF rationale as the pty case above, but the
        // terminal's own constructor already handles the operator's real
        // terminal (see src/term/DESIGN.md), so a plain write_raw suffices.
        pty::write_raw(
            1,
            pty::to_crlf(
                "[Server] Calculator listening via --transport=term. This terminal is now the "
                "spawned shell; run `rmi_client_example --transport=term` from inside it. Exit the "
                "shell to stop.\n"));

        term_proc.wait();
        srv.stop();
        pty::write_raw(1, pty::to_crlf("[Server] stopped.\n"));
        return 0;
      }
      case app::transport_kind::pipe: {
        named_pipe_server_transport transport{FLAGS_name};
        server srv{transport};
        srv.listen();

        std::cout << "[Server] Calculator listening on pipe " << FLAGS_name << " -- press Enter to stop\n"
                  << std::flush;

        std::string line;
        std::getline(std::cin, line);

        srv.stop();
        std::cout << "[Server] stopped." << '\n';
        return 0;
      }
      case app::transport_kind::tcp: {
        auto port = static_cast<uint16_t>(FLAGS_port);
        socket_server_transport transport{FLAGS_host, port};
        server srv{transport};
        srv.listen();

        std::cout << "[Server] Calculator listening on " << FLAGS_host << ":" << FLAGS_port << '\n';
        std::cout << "[Server] Press Enter to stop..." << '\n';

        std::string line;
        std::getline(std::cin, line);

        srv.stop();
        std::cout << "[Server] stopped." << '\n';
        return 0;
      }
    }
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "[Server] error: " << ex.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "[Server] unexpected failure" << '\n';
    return 1;
  }
}
