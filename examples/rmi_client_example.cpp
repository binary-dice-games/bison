// MIT License © 2025 Binary Dice Games
// examples/rmi_client_example.cpp
//
// Standalone RMI client example built directly on rmi::client (rather than
// the client_app scaffold used by bison-cli), to demonstrate manual
// transport construction. Takes the same --transport/--host/--port/--name/
// --timeout/--debugger flags as bison-cli (src/app/cli/main.cpp) so usage is
// consistent across the project -- see docs/examples.md for per-transport
// walkthroughs.

#include "src/app/transport_flags.hpp"
#include "src/bison/bison_flags.hpp"
#include "src/rmi/rmi.hpp"
#include "src/rmi/transport/term_transport.hpp"
#include "src/term/scoped_terminal_config.hpp"

#include <gflags/gflags.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;
using namespace bdg::bison::rmi::shared::constants;

DEFINE_string(transport, "term", "Transport to use: tcp, pipe or term");
DEFINE_string(host, "127.0.0.1", "Server host address (transport=tcp)");
DEFINE_int32(port, 7070, "Server TCP port (transport=tcp)");
DEFINE_string(name, "", "Named-pipe / Unix-socket path (transport=pipe)");
DEFINE_int32(timeout, 30000, "Per-request timeout in milliseconds");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");

extern void wait_for_debugger();

/** @brief Runs the calls shared by every transport against a connected client. */
static void run_calls(client& c) {
  auto calc = c.instantiate(0U, "Calculator"_key).get();
  std::cout << "[Client] connected, object id=" << calc.object_id() << '\n';

  {
    dynamic params;
    params["a"_key] = 10.0f;
    params["b"_key] = 3.0f;
    auto result = calc.call("add"_key, std::move(params)).get();
    float res = result["result"_key];
    std::cout << "[Client] add(10, 3) = " << res << '\n';
  }

  {
    dynamic params;
    params["a"_key] = 100.0f;
    params["b"_key] = 21.0f;
    auto result = calc.call("subtract"_key, std::move(params)).get();
    float res = result["result"_key];
    std::cout << "[Client] subtract(100, 21) = " << res << '\n';
  }

  {
    dynamic params;
    params["a"_key] = 7.0f;
    params["b"_key] = 6.0f;
    auto result = calc.call("multiply"_key, std::move(params)).get();
    float res = result["result"_key];
    std::cout << "[Client] multiply(7, 6) = " << res << '\n';
  }

  {
    dynamic params;
    params["a"_key] = 42.0f;
    params["b"_key] = 2.0f;
    auto result = calc.call("divide"_key, std::move(params)).get();
    float res = result["result"_key];
    std::cout << "[Client] divide(42, 2) = " << res << '\n';
  }

  c.destroy(std::move(calc));
}

static int run_with_transport(std::unique_ptr<client_transport_iface> transport) {
  client c{std::move(transport)};

  dynamic params;
  params["timeout_ms"_key] = int32_t{FLAGS_timeout};
  c.connect(std::move(params));

  run_calls(c);

  c.disconnect();
  std::cout << "[Client] done." << '\n';
  return 0;
}

int main(int argc, char** argv) {
  if (bdg::bison::print_usage(argc, argv, "Standalone RMI Calculator client example.", __FILE__))
    return 0;

  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_debugger) {
    wait_for_debugger();
  }

  try {
    switch (app::selected_transport()) {
      case app::transport_kind::pipe:
        return run_with_transport(std::make_unique<named_pipe_client_transport>(FLAGS_name));
      case app::transport_kind::tcp:
        return run_with_transport(
            std::make_unique<socket_client_transport>(FLAGS_host, static_cast<uint16_t>(FLAGS_port)));
      case app::transport_kind::term: {
        term::scoped_terminal_config stc{{0, 1}};
        auto transport = std::make_unique<term_client_transport>(
            stc.upstream_read_fd(),
            stc.upstream_write_fd(),
            [&stc](std::string_view s) { stc.on_passthrough(s); },
            kDefaultHandshakeTimeout,
            [&stc] { stc.stop_output_pump(); });
        stc.set_output_channel([raw = transport.get()](std::string_view s) { raw->send(s); });
        return run_with_transport(std::move(transport));
      }
    }
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "[Client] failed to connect or execute calls: " << ex.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "[Client] unexpected failure" << '\n';
    return 1;
  }
}
