// MIT License © 2025 Binary Dice Games
// examples/rmi_stdio_client_example.cpp
//
// Standalone RMI client example using subprocess-backed stdio transport.
//
// Two launch modes are supported:
//   --pty  <terminal-cmd> [args...]  Interactive terminal workflow.
//   --pipe <command> [args...]       Direct stdin/stdout pipe workflow.
//
// PTY mode is intended for shells or terminal programs where the user starts
// the server manually inside the spawned terminal, including remote shells
// such as ssh or adb shell.

#include "src/rmi/rmi.hpp"

#if defined(__linux__)

#include <atomic>
#include <cerrno>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <pty.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

enum class launch_mode {
  pty,
  pipe,
};

struct child_process {
  pid_t pid = -1;
  int read_fd = -1;
  int write_fd = -1;
  int stdin_fd = -1;
  launch_mode mode = launch_mode::pty;
  bool stdin_raw_mode_active = false;
  termios saved_stdin_mode{};
  std::shared_ptr<std::atomic<bool>> stop_input =
      std::make_shared<std::atomic<bool>>(false);
  std::thread input_thread;
};

bool enable_client_stdin_raw_mode(child_process& proc) {
  if (!::isatty(STDIN_FILENO)) {
    return false;
  }

  termios tty{};
  if (::tcgetattr(STDIN_FILENO, &tty) != 0) {
    return false;
  }

  proc.saved_stdin_mode = tty;

  // Disable local echo and canonical line buffering, but keep normal output
  // post-processing so '\r' and '\n' continue to format correctly.
  tty.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
  tty.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
  tty.c_cc[VMIN] = 1;
  tty.c_cc[VTIME] = 0;

  if (::tcsetattr(STDIN_FILENO, TCSANOW, &tty) != 0) {
    return false;
  }

  proc.stdin_raw_mode_active = true;
  return true;
}

void restore_client_stdin_mode(child_process& proc) {
  if (!proc.stdin_raw_mode_active) {
    return;
  }

  (void)::tcsetattr(STDIN_FILENO, TCSANOW, &proc.saved_stdin_mode);
  proc.stdin_raw_mode_active = false;
}

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

void close_if_open(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

child_process launch_pipe_mode(int argc, char** argv, int arg_index) {
  int pipe_to_child[2] = {-1, -1};
  int pipe_from_child[2] = {-1, -1};

  if (::pipe(pipe_to_child) != 0 || ::pipe(pipe_from_child) != 0) {
    throw std::runtime_error("pipe() failed");
  }

  const pid_t child = ::fork();
  if (child < 0) {
    close_if_open(pipe_to_child[0]);
    close_if_open(pipe_to_child[1]);
    close_if_open(pipe_from_child[0]);
    close_if_open(pipe_from_child[1]);
    throw std::runtime_error("fork() failed");
  }

  if (child == 0) {
    ::dup2(pipe_to_child[0], STDIN_FILENO);
    ::dup2(pipe_from_child[1], STDOUT_FILENO);
    ::close(pipe_to_child[0]);
    ::close(pipe_to_child[1]);
    ::close(pipe_from_child[0]);
    ::close(pipe_from_child[1]);

    std::vector<char*> child_argv;
    for (int i = arg_index; i < argc; ++i) {
      child_argv.push_back(argv[i]);
    }
    child_argv.push_back(nullptr);
    ::execvp(child_argv[0], child_argv.data());
    _exit(127);
  }

  close_if_open(pipe_to_child[0]);
  close_if_open(pipe_from_child[1]);

  child_process proc;
  proc.pid = child;
  proc.read_fd = pipe_from_child[0];
  proc.write_fd = pipe_to_child[1];
  proc.mode = launch_mode::pipe;
  return proc;
}

child_process launch_pty_mode(int argc, char** argv, int arg_index) {
  int master_fd = -1;
  const pid_t child = ::forkpty(&master_fd, nullptr, nullptr, nullptr);
  if (child < 0) {
    throw std::runtime_error("forkpty() failed");
  }

  if (child == 0) {
    std::vector<char*> child_argv;
    for (int i = arg_index; i < argc; ++i) {
      child_argv.push_back(argv[i]);
    }
    child_argv.push_back(nullptr);
    ::execvp(child_argv[0], child_argv.data());
    _exit(127);
  }

  child_process proc;
  proc.pid = child;
  proc.read_fd = master_fd;
  proc.write_fd = master_fd;
  (void)enable_client_stdin_raw_mode(proc);
  proc.stdin_fd = ::dup(STDIN_FILENO);
  proc.mode = launch_mode::pty;
  auto stop_input = proc.stop_input;
  const int stdin_fd = proc.stdin_fd;
  const int write_fd = proc.write_fd;

  // Input thread: read user's stdin and write to PTY
  proc.input_thread = std::thread([stop_input, stdin_fd, write_fd] {
    char buf[1024];
    while (!stop_input->load()) {
      const auto n = ::read(stdin_fd, buf, sizeof(buf));
      if (n > 0) {
        if (!write_all_fd(write_fd, buf, static_cast<size_t>(n))) {
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

  return proc;
}

void cleanup_child_process(child_process& proc) {
  if (proc.mode == launch_mode::pty && proc.write_fd >= 0) {
    static constexpr char kExitCmd[] = "exit\n";
    (void)write_all_fd(proc.write_fd, kExitCmd, sizeof(kExitCmd) - 1);
  }

  if (proc.stop_input) {
    proc.stop_input->store(true);
  }
  close_if_open(proc.stdin_fd);
  if (proc.input_thread.joinable()) {
    proc.input_thread.join();
  }
  restore_client_stdin_mode(proc);
  if (proc.write_fd == proc.read_fd) {
    close_if_open(proc.write_fd);
    proc.read_fd = -1;
  } else {
    close_if_open(proc.write_fd);
    close_if_open(proc.read_fd);
  }

  if (proc.pid > 0) {
    int status = 0;
    (void)::waitpid(proc.pid, &status, 0);
    proc.pid = -1;
  }
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
  std::cerr << "[Client] PTY/pipe launcher mode is Linux/POSIX only.\n";
  return 1;
#else
  if (argc < 2) {
    std::cerr << "Usage: rmi_stdio_client_example [--pty|--pipe] <command>"
                 " [args...]\n";
    std::cerr << "Example: rmi_stdio_client_example --pty bash\n";
    std::cerr << "Example: rmi_stdio_client_example --pty ssh user@host\n";
    std::cerr << "Example: rmi_stdio_client_example --pipe ssh user@host"
                 " /path/to/rmi_stdio_server_example\n";
    return 1;
  }

  launch_mode mode = launch_mode::pty;
  int arg_index = 1;

  if (std::string_view{argv[1]} == "--pipe") {
    mode = launch_mode::pipe;
    arg_index = 2;
  } else if (std::string_view{argv[1]} == "--pty") {
    mode = launch_mode::pty;
    arg_index = 2;
  }

  if (arg_index >= argc) {
    std::cerr << "[Client] missing subprocess command.\n";
    return 1;
  }

  child_process proc;
  try {
    proc = (mode == launch_mode::pipe) ? launch_pipe_mode(argc, argv, arg_index)
                                       : launch_pty_mode(argc, argv, arg_index);
  } catch (const std::exception& ex) {
    std::cerr << "[Client] failed to launch subprocess: " << ex.what() << '\n';
    return 1;
  }

  try {
    stdio_client_transport transport{proc.read_fd, proc.write_fd};
    client c{std::move(transport)};

    dynamic connect_params;
    connect_params["mode"_key] = std::string{"dcs"};
    connect_params["handshake_timeout_ms"_key] =
        (mode == launch_mode::pty) ? int32_t{300000} : int32_t{10000};
    connect_params["mirror_plaintext_to_stderr"_key] = true;

    if (mode == launch_mode::pty) {
      std::cerr << "[Client] terminal subprocess started.\n";
      std::cerr << "[Client] Start rmi_stdio_server_example inside that"
                   " terminal.\n";
      std::cerr << "[Client] Waiting for HELLO...\n";
    } else {
      std::cerr << "[Client] subprocess started. Waiting for HELLO...\n";
    }

    c.connect(std::move(connect_params));
    std::cerr << "[Client] HELLO received. RMI channel connected.\n";

    auto calc = c.instantiate("Calculator"_key).get();
    std::cerr << "[Client] instantiated Calculator, id=" << calc.object_id()
              << '\n';

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

    cleanup_child_process(proc);
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "[Client] failed: " << ex.what() << '\n';
    cleanup_child_process(proc);
    return 1;
  } catch (...) {
    std::cerr << "[Client] unexpected failure.\n";
    cleanup_child_process(proc);
    return 1;
  }
#endif
}
