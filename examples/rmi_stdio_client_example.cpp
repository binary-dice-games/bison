// MIT License © 2025 Binary Dice Games
// examples/rmi_stdio_client_example.cpp
//
// Standalone RMI client example using Linux PTY launcher workflow.

#include "src/rmi/rmi.hpp"

#if defined(__linux__)

#include <cerrno>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <pty.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

bool write_all_fd(int fd, const char* data, size_t size) {
  size_t sent = 0;
  while (sent < size) {
    const auto n = ::write(fd, data + sent, size - sent);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

} // namespace

#endif

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;

int main(int argc, char** argv) {
#if !defined(__linux__)
  (void)argc;
  (void)argv;
  std::cerr << "[Client] PTY launcher mode is Linux-only. Use Linux or WSL.\n";
  return 1;
#else
  if (argc < 2) {
    std::cerr << "Usage: rmi_stdio_client_example <command> [args...]\n";
    std::cerr << "Example: rmi_stdio_client_example bash\n";
    std::cerr << "Example: rmi_stdio_client_example ssh user@host\n";
    return 1;
  }

  int master_fd = -1;
  const pid_t child = ::forkpty(&master_fd, nullptr, nullptr, nullptr);
  if (child < 0) {
    std::cerr << "[Client] forkpty failed.\n";
    return 1;
  }

  if (child == 0) {
    std::vector<char*> child_argv;
    for (int i = 1; i < argc; ++i) {
      child_argv.push_back(argv[i]);
    }
    child_argv.push_back(nullptr);
    ::execvp(child_argv[0], child_argv.data());
    _exit(127);
  }

  const int stdin_fd = ::dup(STDIN_FILENO);
  std::atomic<bool> stop_input{false};
  std::thread input_thread([&] {
    char buf[1024];
    while (!stop_input.load()) {
      const auto n = ::read(stdin_fd, buf, sizeof(buf));
      if (n > 0) {
        if (!write_all_fd(master_fd, buf, static_cast<size_t>(n))) {
          break;
        }
        continue;
      }
      if (n == 0) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      break;
    }
  });

  try {
    stdio_client_transport transport{master_fd, master_fd};
    client c{std::move(transport)};

    dynamic connect_params;
    connect_params["mode"_key] = std::string{"auto"};
    connect_params["handshake_timeout_ms"_key] = int32_t{300000};
    connect_params["mirror_plaintext_to_stderr"_key] = true;

    std::cerr << "[Client] PTY command started.\n";
    std::cerr << "[Client] Run rmi_stdio_server_example in that shell.\n";
    std::cerr << "[Client] Waiting for HELLO...\n";

    c.connect(std::move(connect_params));
    std::cerr << "[Client] HELLO received. RMI channel connected.\n";

    auto calc = c.instantiate("Calculator"_key).get();
    std::cerr << "[Client] connected, object id=" << calc.object_id() << '\n';

    {
      dynamic params;
      params["a"_key] = 10.0f;
      params["b"_key] = 3.0f;
      auto result = calc.call("add"_key, std::move(params)).get();
      std::cerr << "[Client] add(10, 3) = " << float(result["result"_key])
                << '\n';
    }

    {
      dynamic params;
      params["a"_key] = 21.0f;
      params["b"_key] = 7.0f;
      auto result = calc.call("divide"_key, std::move(params)).get();
      std::cerr << "[Client] divide(21, 7) = " << float(result["result"_key])
                << '\n';
    }

    c.destroy(std::move(calc));
    c.disconnect();

    std::cerr << "[Client] done.\n";

    const std::string exit_cmd = "exit\n";
    (void)write_all_fd(master_fd, exit_cmd.data(), exit_cmd.size());

    stop_input.store(true);
    ::close(stdin_fd);
    if (input_thread.joinable()) {
      input_thread.join();
    }

    int status = 0;
    (void)::waitpid(child, &status, 0);
    ::close(master_fd);

    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "[Client] failed: " << ex.what() << '\n';

    stop_input.store(true);
    ::close(stdin_fd);
    if (input_thread.joinable()) {
      input_thread.join();
    }
    ::close(master_fd);
    int status = 0;
    (void)::waitpid(child, &status, 0);

    return 1;
  } catch (...) {
    std::cerr << "[Client] unexpected failure.\n";

    stop_input.store(true);
    ::close(stdin_fd);
    if (input_thread.joinable()) {
      input_thread.join();
    }
    ::close(master_fd);
    int status = 0;
    (void)::waitpid(child, &status, 0);

    return 1;
  }
#endif
}
