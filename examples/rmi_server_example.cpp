// MIT License © 2025 Binary Dice Games
// examples/rmi_server_example.cpp
//
// Standalone RMI server example. Uses socket transport by default; pass
// --pty as the first argument to instead serve over a forked pseudo-terminal
// (Linux or MSYS2 only — see src/pty/DESIGN.md and docs/building.md for the
// MSYS2 MSYS-shell requirement).

#include "src/pty/pty_process.hpp"
#include "src/pty/pty_write.hpp"
#include "src/rmi/rmi.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;
using namespace bdg::bison::rmi::shared::constants;

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

static int run_pty(int argc, char** argv) {
  (void)argc;
  (void)argv;
  register_calculator();

  pty::pty_process pty_proc;
  pty_proc.start_pump();
  stdio_server_transport transport{pty_proc.master_fd(), pty_proc.master_fd()};
  server srv{transport};
  srv.listen();

  // pty_proc's constructor already put the operator's own real terminal in
  // raw mode (for pump_loop() — see src/pty/DESIGN.md), which strips \r
  // from plain std::cout output. Writing directly via pty::write_raw here
  // (rather than through std::cout, which stdio_print_passthrough is also
  // concurrently writing to from another thread to forward pty-master bytes
  // verbatim) avoids both corrupting that forwarded text and racing it —
  // see pty_write.hpp's doc comment.
  pty::write_raw(1, pty::to_crlf("[Server] Calculator listening via --pty. This terminal is now the spawned "
                                  "shell; run `rmi_client_example --pty` from inside it. Exit the shell to stop.\n"));

  pty_proc.wait();
  srv.stop();
  pty::write_raw(1, pty::to_crlf("[Server] stopped.\n"));
  return 0;
}

int main(int argc, char** argv) {
  if (argc > 1 && std::string{argv[1]} == "--pty") {
    return run_pty(argc, argv);
  }

  std::string host = "127.0.0.1";
  uint16_t port = 7070;

  if (argc > 1) {
    host = argv[1];
  }
  if (argc > 2) {
    const auto parsed = std::strtoul(argv[2], nullptr, 10);
    if (parsed > 0 && parsed <= 65535) {
      port = static_cast<uint16_t>(parsed);
    }
  }

  register_calculator();

  socket_server_transport transport{host, port};
  server srv{transport};
  srv.listen();

  std::cout << "[Server] Calculator listening on " << host << ":" << port << '\n';
  std::cout << "[Server] Press Enter to stop..." << '\n';

  std::string line;
  std::getline(std::cin, line);

  srv.stop();
  std::cout << "[Server] stopped." << '\n';

  return 0;
}
