// MIT License © 2025 Binary Dice Games
/**
 * @file pty_client_app.cpp
 * @brief Reusable PTY/pipe client application scaffold implementation.
 */
#include "src/rmi/client/pty_client_app.hpp"

#if defined(__linux__)

#include "src/rmi/transport/stdio_transport.hpp"

#include <poll.h>
#include <pty.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace bdg::bison::rmi::apps {

namespace {

struct child_process {
  pid_t pid = -1;
  int read_fd = -1;
  int write_fd = -1;
  int stdin_fd = -1;
  pty_client_application::launch_mode mode =
      pty_client_application::launch_mode::pty;
  bool stdin_raw_mode_active = false;
  termios saved_stdin_mode{};
  std::shared_ptr<std::atomic<bool>> stop_input =
      std::make_shared<std::atomic<bool>>(false);
  std::thread input_thread;
};

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

bool enable_client_stdin_raw_mode(child_process& proc) {
  if (!::isatty(STDIN_FILENO)) {
    return false;
  }

  termios tty{};
  if (::tcgetattr(STDIN_FILENO, &tty) != 0) {
    return false;
  }

  proc.saved_stdin_mode = tty;
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
  proc.mode = pty_client_application::launch_mode::pipe;
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
  proc.mode = pty_client_application::launch_mode::pty;

  auto stop_input = proc.stop_input;
  const int stdin_fd = proc.stdin_fd;
  const int write_fd = proc.write_fd;

  proc.input_thread = std::thread([stop_input, stdin_fd, write_fd] {
    char buf[1024];
    while (!stop_input->load()) {
      pollfd pfd{};
      pfd.fd = stdin_fd;
      pfd.events = POLLIN;

      const int poll_result = ::poll(&pfd, 1, 100);
      if (poll_result == 0) {
        continue;
      }
      if (poll_result < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
        break;
      }
      if ((pfd.revents & (POLLIN | POLLHUP)) == 0) {
        continue;
      }

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

bool wait_for_child_exit(child_process& proc) {
  if (proc.pid <= 0) {
    return true;
  }

  while (true) {
    int status = 0;
    const pid_t wait_result = ::waitpid(proc.pid, &status, WNOHANG);
    if (wait_result == proc.pid) {
      proc.pid = -1;
      return true;
    }
    if (wait_result == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds{25});
      continue;
    }
    if (wait_result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
}

void cleanup_child_process(child_process& proc, bool request_exit) {
  if (request_exit && proc.mode == pty_client_application::launch_mode::pty &&
      proc.write_fd >= 0) {
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

void pty_client_application::on_usage() const {
  std::cerr << "Usage: rmi_pty_client [--pty|--pipe] <command> [args...]\n";
  std::cerr << "Example: rmi_pty_client --pty bash\n";
  std::cerr << "Example: rmi_pty_client --pty ssh user@host\n";
  std::cerr << "Example: rmi_pty_client --pipe ssh user@host /path/to/server\n";
}

void pty_client_application::on_subprocess_started(
    const run_context& ctx) const {
  if (ctx.mode == launch_mode::pty) {
    std::cerr << "[Client] terminal subprocess started.\n";
    std::cerr
        << "[Client] Start rmi_stdio_server_example inside that terminal.\n";
  } else {
    std::cerr << "[Client] subprocess started.\n";
  }
}

void pty_client_application::on_waiting_for_hello(const run_context&) const {
  std::cerr << "[Client] Waiting for HELLO...\n";
}

void pty_client_application::on_connected(const run_context&) const {
  std::cerr << "[Client] HELLO received. RMI channel connected.\n";
}

void pty_client_application::on_waiting_for_console_close(
    const run_context& ctx) const {
  if (ctx.mode == launch_mode::pty) {
    std::cerr << "[Client] Console running. Close it to end the session.\n";
  }
}

void pty_client_application::on_connect_params(
    bison::dynamic& params,
    const run_context& ctx) const {
  params["mode"_key] = std::string{"dcs"};
  params["handshake_timeout_ms"_key] =
      (ctx.mode == launch_mode::pty) ? int32_t{300000} : int32_t{10000};
  params["mirror_plaintext_to_stderr"_key] = true;
}

int pty_client_application::on_session(client&, const run_context&) {
  return 0;
}

void pty_client_application::on_error(const std::string& message) const {
  std::cerr << "[Client] failed: " << message << '\n';
}

int pty_client_application::run(int argc, char** argv) {
  if (argc < 2) {
    on_usage();
    return 1;
  }

  run_context ctx;
  int arg_index = 1;

  if (std::string_view{argv[1]} == "--pipe") {
    ctx.mode = launch_mode::pipe;
    arg_index = 2;
  } else if (std::string_view{argv[1]} == "--pty") {
    ctx.mode = launch_mode::pty;
    arg_index = 2;
  }

  if (arg_index >= argc) {
    on_error("missing subprocess command");
    on_usage();
    return 1;
  }

  ctx.command = argv[arg_index];

  child_process proc;
  try {
    proc = (ctx.mode == launch_mode::pipe)
        ? launch_pipe_mode(argc, argv, arg_index)
        : launch_pty_mode(argc, argv, arg_index);
  } catch (const std::exception& ex) {
    on_error(std::string{"failed to launch subprocess: "} + ex.what());
    return 1;
  }

  try {
    transport::stdio_client_transport transport{proc.read_fd, proc.write_fd};
    client rmi_client{std::move(transport)};

    bison::dynamic connect_params;
    on_connect_params(connect_params, ctx);

    on_subprocess_started(ctx);
    on_waiting_for_hello(ctx);

    rmi_client.connect(std::move(connect_params));
    on_connected(ctx);

    const int session_code = on_session(rmi_client, ctx);

    if (ctx.mode == launch_mode::pty) {
      on_waiting_for_console_close(ctx);
      (void)wait_for_child_exit(proc);
    }

    rmi_client.disconnect();
    cleanup_child_process(proc, false);
    return session_code;
  } catch (const std::exception& ex) {
    on_error(ex.what());
    cleanup_child_process(proc, true);
    return 1;
  } catch (...) {
    on_error("unexpected failure");
    cleanup_child_process(proc, true);
    return 1;
  }
}

} // namespace bdg::bison::rmi::apps

#else

namespace bdg::bison::rmi::apps {

void pty_client_application::on_usage() const {
  std::cerr << "PTY client mode is Linux/POSIX only.\n";
}

void pty_client_application::on_subprocess_started(const run_context&) const {}

void pty_client_application::on_waiting_for_hello(const run_context&) const {}

void pty_client_application::on_connected(const run_context&) const {}

void pty_client_application::on_waiting_for_console_close(
    const run_context&) const {}

void pty_client_application::on_connect_params(
    bison::dynamic&,
    const run_context&) const {}

int pty_client_application::on_session(client&, const run_context&) {
  return 0;
}

void pty_client_application::on_error(const std::string& message) const {
  std::cerr << "[Client] failed: " << message << '\n';
}

int pty_client_application::run(int argc, char** argv) {
  (void)argc;
  (void)argv;
  on_usage();
  return 1;
}

} // namespace bdg::bison::rmi::apps

#endif
