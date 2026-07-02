// MIT License © 2025 Binary Dice Games
// examples/rmi_client_example.cpp
//
// Standalone RMI client example. Uses socket transport by default; pass
// --pty as the first argument to instead connect over this process's own
// inherited stdio (for use inside a `rmi_server_example --pty` session —
// see src/pty/DESIGN.md).

#include "src/pty/crlf_output_guard.hpp"
#include "src/pty/raw_mode_guard.hpp"
#include "src/rmi/rmi.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;
using namespace bdg::bison::rmi::shared::constants;

/** @brief Runs the calls shared by both transports against a connected client. */
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

static int run_pty() {
  try {
    pty::raw_mode_guard raw{0};
    // Raw mode strips \r from this process's own std::cout/std::cerr output
    // too (turning OPOST off is global to the fd) — compensate, or the
    // "[Client] ..." lines below stairstep across the screen instead of
    // starting at the left margin. See crlf_output_guard's doc comment.
    pty::crlf_output_guard output_guard;
    client c{std::make_unique<stdio_client_transport>(0, 1)};
    c.connect();

    run_calls(c);

    c.disconnect();
    std::cout << "[Client] done." << '\n';
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "[Client] failed to connect or execute calls over --pty: " << ex.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "[Client] unexpected failure while connecting over --pty" << '\n';
    return 1;
  }
}

int main(int argc, char** argv) {
  if (argc > 1 && std::string{argv[1]} == "--pty") {
    return run_pty();
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

  try {
    socket_client_transport transport{host, port};
    client c{std::move(transport)};
    c.connect();

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
    c.disconnect();
    std::cout << "[Client] done." << '\n';
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "[Client] failed to connect or execute calls at " << host << ":" << port << ": " << ex.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "[Client] unexpected failure while connecting to " << host << ":" << port << '\n';
    return 1;
  }
}
