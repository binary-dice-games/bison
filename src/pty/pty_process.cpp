// MIT License © 2025 Binary Dice Games
/**
 * @file pty_process.cpp
 * @brief Linux/MSYS2 implementation of pty_process using forkpty().
 */
#include "src/pty/pty_process.hpp"

#include <pty.h>

#include <poll.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#else
#include <fcntl.h>
#endif
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace bdg::bison::pty {

/** @brief Saved real-terminal state and the pump thread's shutdown descriptors. */
struct pty_process_state {
  struct termios saved_termios{};
  bool termios_saved{false};
  int stop_read_fd{-1};
#if !defined(__linux__)
  int stop_write_fd{-1}; // Track the write side of the pipe on MSYS2/Cygwin
#endif
};

pty_process::pty_process(const std::string& cmd) : state_(std::make_unique<pty_process_state>()) {
  struct winsize ws{};
  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0) {
    ws.ws_col = 80;
    ws.ws_row = 24;
  }

  const pid_t pid = forkpty(&master_fd_, nullptr, nullptr, &ws);
  if (pid < 0)
    throw std::runtime_error(std::string{"pty_process: forkpty failed: "} + std::strerror(errno));

  if (pid == 0) {
    // Child: forkpty() has already made the pty slave our controlling
    // terminal and wired it to fd 0/1/2.
    std::string shell = cmd;
    if (shell.empty()) {
      const char* env_shell = std::getenv("SHELL");
      shell = (env_shell != nullptr && *env_shell != '\0') ? env_shell : "/bin/sh";
    }
    execl(shell.c_str(), shell.c_str(), static_cast<char*>(nullptr));
    _exit(127); // exec failed
  }

  child_pid_ = pid;

  // Raw mode applies to *our own* real terminal (fd 0), not the spawned
  // pty's slave side. That is what makes pump_loop()'s read() deliver
  // keystrokes immediately (ICANON off) and forward Ctrl-C as a literal
  // 0x03 byte (ISIG off) instead of killing this process. The pty slave
  // keeps its default termios, so the kernel's line discipline on that side
  // turns the forwarded 0x03 into a real SIGINT for the child.
  if (tcgetattr(STDIN_FILENO, &state_->saved_termios) == 0) {
    state_->termios_saved = true;
    struct termios raw = state_->saved_termios;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
  }

#if defined(__linux__)
  state_->stop_read_fd = eventfd(0, EFD_NONBLOCK);
#else
  // Fallback: Create a POSIX self-pipe to trigger unblocking poll() on MSYS2
  int pipe_fds[2];
  if (pipe(pipe_fds) == 0) {
    state_->stop_read_fd = pipe_fds[0]; // Read side
    state_->stop_write_fd = pipe_fds[1]; // Write side
    // Make the read end non-blocking
    fcntl(state_->stop_read_fd, F_SETFL, fcntl(state_->stop_read_fd, F_GETFL, 0) | O_NONBLOCK);
  }
#endif
}

pty_process::~pty_process() {
  pump_running_.store(false);
  if (state_->stop_read_fd >= 0) {
    uint64_t one = 1;
#if defined(__linux__)
    static_cast<void>(write(state_->stop_read_fd, &one, sizeof(one)));
#else
    // Write a byte into the pipe to safely break out of poll()
    static_cast<void>(write(state_->stop_write_fd, &one, 1));
#endif
  }
  if (pump_thread_.joinable())
    pump_thread_.join();

  if (state_->termios_saved)
    tcsetattr(STDIN_FILENO, TCSANOW, &state_->saved_termios);

  if (state_->stop_read_fd >= 0)
    close(state_->stop_read_fd);

#if !defined(__linux__)
  if (state_->stop_write_fd >= 0)
    close(state_->stop_write_fd);
#endif

  if (master_fd_ >= 0)
    close(master_fd_);

  if (child_pid_ > 0) {
    int status{};
    waitpid(child_pid_, &status, 0);
  }
}

int pty_process::master_fd() const {
  return master_fd_;
}

void pty_process::start_pump() {
  if (pump_running_.exchange(true))
    return;
  pump_thread_ = std::thread([this] { pump_loop(); });
}

int pty_process::wait() {
  if (child_pid_ <= 0)
    return -1;
  int status{};
  if (waitpid(child_pid_, &status, 0) < 0)
    return -1;
  child_pid_ = -1; // reaped; destructor must not wait again
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return status;
}

void pty_process::pump_loop() {
  char buf[4096];
  struct pollfd fds[2]{};
  fds[0].fd = STDIN_FILENO;
  fds[0].events = POLLIN;
  fds[1].fd = state_->stop_read_fd;
  fds[1].events = POLLIN;

  while (pump_running_.load()) {
    fds[0].revents = 0;
    fds[1].revents = 0;
    const int r = poll(fds, 2, -1);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if ((fds[1].revents & POLLIN) != 0)
      break; // stop signaled

    if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if (n <= 0)
        break;
      ssize_t written = 0;
      while (written < n) {
        const ssize_t w = write(master_fd_, buf + written, static_cast<size_t>(n - written));
        if (w <= 0)
          break;
        written += w;
      }
    }
  }
}

} // namespace bdg::bison::pty