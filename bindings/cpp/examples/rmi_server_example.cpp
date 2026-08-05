// MIT License © 2025 Binary Dice Games
// bindings/cpp/examples/rmi_server_example.cpp
//
// RMI server example using the header-only C++ ABI binding
// (bindings/cpp/include/bison/rmi.hpp). Mirrors
// examples/rmi_abi_server_example.cpp and
// bindings/python/examples/rmi_server_example.py. Command-line flags match
// the --transport/--host/--port/--name convention used across the other
// examples.
//
// Run: ./rmi_cpp_server_example [--transport=tcp|pipe] [--host=HOST] [--port=PORT] [--name=PATH]

#include "bison/rmi.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace bdg::bison::abi;

namespace {

void print_usage(const char* program) {
  fprintf(stderr, "Usage: %s [--transport=tcp|pipe] [--host=HOST] [--port=PORT] [--name=PATH]\n", program);
}

void register_calculator() {
  dynamic proto{"Calculator"_key};
  proto.addMethod("add"_key, [](dynamic&, const dynamic& params, dynamic& result) {
    result["result"_key] = params["a"_key].as<float>() + params["b"_key].as<float>();
  });
  proto.addMethod("subtract"_key, [](dynamic&, const dynamic& params, dynamic& result) {
    result["result"_key] = params["a"_key].as<float>() - params["b"_key].as<float>();
  });
  proto.addMethod("multiply"_key, [](dynamic&, const dynamic& params, dynamic& result) {
    result["result"_key] = params["a"_key].as<float>() * params["b"_key].as<float>();
  });
  proto.addMethod("divide"_key, [](dynamic&, const dynamic& params, dynamic& result) {
    float a = params["a"_key].as<float>();
    float b = params["b"_key].as<float>();
    if (b == 0.0f) {
      result["error"_key] = std::string{"division by zero"};
      result["result"_key] = 0.0f;
    } else {
      result["result"_key] = a / b;
    }
  });
  dynamic::addClass(proto);
}

} // namespace

int main(int argc, char** argv) {
  std::string transport = "tcp";
  std::string host = "0.0.0.0";
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

  register_calculator();

  try {
    rmi::server server = transport == "tcp" ? rmi::server::tcp(host, port)
        : transport == "pipe"               ? rmi::server::pipe(name)
                                            : throw std::runtime_error("unsupported --transport: " + transport);
    server.listen();

    if (transport == "pipe")
      printf("[Server] Calculator listening on pipe %s\n", name.c_str());
    else
      printf("[Server] Calculator listening on %s:%u\n", host.c_str(), static_cast<unsigned>(port));
    printf("[Server] Press Enter to stop...\n");
    fflush(stdout);

    getchar();

    server.stop();
    printf("[Server] stopped.\n");
  } catch (const std::exception& e) {
    fprintf(stderr, "[Server] error: %s\n", e.what());
    return 1;
  }
  return 0;
}
