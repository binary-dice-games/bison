// MIT License © 2025 Binary Dice Games
// bindings/cpp/examples/rmi_client_example.cpp
//
// RMI client example using the header-only C++ ABI binding
// (bindings/cpp/include/bison/rmi.hpp). Mirrors
// examples/rmi_abi_client_example.cpp and
// bindings/python/examples/rmi_client_example.py. Command-line flags mirror
// the --transport/--host/--port/--name names used across the other
// examples. --transport=term takes no --host/--port/--name: like
// bison-cli, this process wraps its own already-connected fd 0/1 rather
// than spawning anything.
//
// Run rmi_server_example (or any Calculator server, e.g.
// rmi_abi_server_example) with matching flags before starting this client.
//
// Run: ./rmi_cpp_client_example [--transport=tcp|pipe|term] [--host=HOST] [--port=PORT] [--name=PATH]

#include "bison/rmi.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace bdg::bison::abi;

namespace {

void print_usage(const char* program) {
  fprintf(stderr, "Usage: %s [--transport=tcp|pipe|term] [--host=HOST] [--port=PORT] [--name=PATH]\n", program);
}

// bdg::bison::abi::key_t must stay qualified here: glibc's <sys/types.h>
// (pulled in transitively) also defines a global `key_t` typedef, and the
// `using namespace bdg::bison::abi;` above makes a bare `key_t` ambiguous
// between the two (same pitfall the internal C++ codebase's own tests
// document -- see tests/bison_c_tests.cpp).
float call_result(rmi::proxy& calc, bdg::bison::abi::key_t method, float a, float b) {
  dynamic params;
  params["a"_key] = a;
  params["b"_key] = b;
  dynamic result = calc.call(method, params);
  return result["result"_key].as<float>();
}

} // namespace

int main(int argc, char** argv) {
  std::string transport = "tcp";
  std::string host = "127.0.0.1";
  uint16_t port = 7070;
  std::string name;

  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
      print_usage(argv[0]);
      return 0;
    } else if (std::strncmp(arg, "--transport=", 12) == 0) {
      transport = arg + 12;
    } else if (std::strncmp(arg, "--host=", 7) == 0) {
      host = arg + 7;
    } else if (std::strncmp(arg, "--port=", 7) == 0) {
      port = static_cast<uint16_t>(std::strtoul(arg + 7, nullptr, 10));
    } else if (std::strncmp(arg, "--name=", 7) == 0) {
      name = arg + 7;
    } else {
      fprintf(stderr, "Unknown option: %s\n", arg);
      print_usage(argv[0]);
      return 1;
    }
  }

  try {
    rmi::client client = transport == "tcp" ? rmi::client::tcp(host, port)
        : transport == "pipe"               ? rmi::client::pipe(name)
        : transport == "term"               ? rmi::client::term()
                                            : throw std::runtime_error("unsupported --transport: " + transport);
    client.connect();

    rmi::proxy calc = client.instantiate("Calculator"_key);
    printf("[Client] connected\n");

    printf("[Client] add(10, 3) = %.0f\n", call_result(calc, "add"_key, 10.0f, 3.0f));
    printf("[Client] subtract(100, 21) = %.0f\n", call_result(calc, "subtract"_key, 100.0f, 21.0f));
    printf("[Client] multiply(7, 6) = %.0f\n", call_result(calc, "multiply"_key, 7.0f, 6.0f));
    printf("[Client] divide(42, 2) = %.0f\n", call_result(calc, "divide"_key, 42.0f, 2.0f));

    // calc's destructor sends the destroy request; client's destructor
    // disconnects and releases the underlying rmi_client_handle.
    printf("[Client] done.\n");
  } catch (const std::exception& e) {
    fprintf(stderr, "[Client] error: %s\n", e.what());
    return 1;
  }
  return 0;
}
