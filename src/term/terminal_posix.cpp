// MIT License © 2025 Binary Dice Games
/**
 * @file terminal_posix.cpp
 * @brief POSIX (Linux/MSYS2/macOS) implementation of terminal using forkpty().
 */
#include "src/term/terminal.hpp"

#if defined(__APPLE__)
// macOS/BSD declare forkpty()/openpty() in <util.h>; glibc and MSYS2 use
// <pty.h>. The forkpty() implementation itself lives in libSystem on macOS,
// so no separate libutil link is needed (see the CMakeLists.txt guard).
#include <util.h>
#else
#include <pty.h>
#endif

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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace bdg::bison::term {

namespace {

/** @brief Rewrites `'\n'` to `"\r\n"` in @p text, returning the result. */
std::string to_crlf(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    if (c == '\n')
      out.push_back('\r');
    out.push_back(c);
  }
  return out;
}

} // namespace

struct terminal_state {
  struct termios saved_termios{};
  bool termios_saved{false};
  int stop_read_fd{-1};
#if !defined(__linux__)
  int stop_write_fd{-1};
#endif
  int child_pid{-1};

  // stdout/stderr redirect (see terminal.hpp's class doc comment).
  int saved_stdout_fd{-1};
  int stdio_pipe_read_fd{-1};
  int stdio_pipe_write_fd{-1};
  std::thread stdio_pump_thread;
};

terminal::terminal(const std::string& cmd, const std::string& prompt_label)
    : state_(std::make_unique<terminal_state>()) {
  if (!prompt_label.empty()) {
    // Set in the parent so forkpty()'s child inherits them via environ
    // (execl() below execs with the current environment, no explicit envp
    // needed). PS1 covers shells with no rc file (e.g. plain /bin/sh);
    // PROMPT_COMMAND re-asserts the prompt on every draw, after bash's own
    // .bashrc has already (unconditionally) set PS1 itself.
    // Bold green label, bold blue cwd -- matches the color scheme typical
    // distro .bashrc files use for \u@\h:\w. \[ \] mark the escape codes as
    // zero-width for readline's line-length tracking (required so bash
    // doesn't misjudge the cursor position and garble in-line editing).
    const std::string ps1 =
        "\\[\\033[01;32m\\]" + prompt_label + "\\[\\033[00m\\]:\\[\\033[01;34m\\]\\w\\[\\033[00m\\]\\$ ";
    setenv("PS1", ps1.c_str(), 1);
    setenv("PROMPT_COMMAND", ("PS1='" + ps1 + "'").c_str(), 1);
  }

  struct winsize ws{};
  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0) {
    ws.ws_col = 80;
    ws.ws_row = 24;
  }

  int master_fd = -1;
  const pid_t pid = forkpty(&master_fd, nullptr, nullptr, &ws);
  if (pid < 0)
    throw std::runtime_error(std::string{"terminal: forkpty failed: "} + std::strerror(errno));

  if (pid == 0) {
    std::string shell = cmd;
    if (shell.empty()) {
      const char* env_shell = std::getenv("SHELL");
      shell = (env_shell != nullptr && *env_shell != '\0') ? env_shell : "/bin/sh";
    }
    execl(shell.c_str(), shell.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }

  read_handle_ = master_fd;
  write_handle_ = master_fd;
  state_->child_pid = pid;

  // Raw mode applies to our own real terminal (fd 0), not the pty slave
  if (tcgetattr(STDIN_FILENO, &state_->saved_termios) == 0) {
    state_->termios_saved = true;
    struct termios raw = state_->saved_termios;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
  }

  // Redirect fd 1/2 through a pipe + background byte-level CRLF translator
  // for the object's lifetime (see terminal.hpp's class doc comment).
  state_->saved_stdout_fd = dup(STDOUT_FILENO);
  int stdio_pipe_fds[2];
  if (state_->saved_stdout_fd >= 0 && pipe(stdio_pipe_fds) == 0) {
    state_->stdio_pipe_read_fd = stdio_pipe_fds[0];
    state_->stdio_pipe_write_fd = stdio_pipe_fds[1];
    fflush(stdout);
    fflush(stderr);
    dup2(state_->stdio_pipe_write_fd, STDOUT_FILENO);
    dup2(state_->stdio_pipe_write_fd, STDERR_FILENO);
    state_->stdio_pump_thread = std::thread([this] { stdio_pump_loop(); });
  }

#if defined(__linux__)
  state_->stop_read_fd = eventfd(0, EFD_NONBLOCK);
#else
  int pipe_fds[2];
  if (pipe(pipe_fds) == 0) {
    state_->stop_read_fd = pipe_fds[0];
    state_->stop_write_fd = pipe_fds[1];
    fcntl(state_->stop_read_fd, F_SETFL, fcntl(state_->stop_read_fd, F_GETFL, 0) | O_NONBLOCK);
  }
#endif
}

terminal::~terminal() {
  pump_running_.store(false);
  if (state_->stop_read_fd >= 0) {
    uint64_t one = 1;
#if defined(__linux__)
    static_cast<void>(write(state_->stop_read_fd, &one, sizeof(one)));
#else
    static_cast<void>(write(state_->stop_write_fd, &one, 1));
#endif
  }
  if (pump_thread_.joinable())
    pump_thread_.join();

  if (state_->stdio_pipe_write_fd >= 0) {
    fflush(stdout);
    fflush(stderr);
    if (state_->saved_stdout_fd >= 0) {
      dup2(state_->saved_stdout_fd, STDOUT_FILENO);
      dup2(state_->saved_stdout_fd, STDERR_FILENO);
    }
    close(state_->stdio_pipe_write_fd); // last writer closed -> pump thread sees EOF
    if (state_->stdio_pump_thread.joinable())
      state_->stdio_pump_thread.join();
    close(state_->stdio_pipe_read_fd);
    if (state_->saved_stdout_fd >= 0)
      close(state_->saved_stdout_fd);
  }

  if (state_->termios_saved)
    tcsetattr(STDIN_FILENO, TCSANOW, &state_->saved_termios);

  if (state_->stop_read_fd >= 0)
    close(state_->stop_read_fd);
#if !defined(__linux__)
  if (state_->stop_write_fd >= 0)
    close(state_->stop_write_fd);
#endif

  if (read_handle_ >= 0)
    close(read_handle_); // read_handle_ == write_handle_ (one pty master fd)

  if (state_->child_pid > 0) {
    int status{};
    waitpid(state_->child_pid, &status, 0);
  }
}

int terminal::read_handle() const {
  return read_handle_;
}

int terminal::write_handle() const {
  return write_handle_;
}

void terminal::start_pump() {
  if (pump_running_.exchange(true))
    return;
  pump_thread_ = std::thread([this] { pump_loop(); });
}

int terminal::wait() {
  if (state_->child_pid <= 0)
    return -1;
  int status{};
  if (waitpid(state_->child_pid, &status, 0) < 0)
    return -1;
  state_->child_pid = -1;
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return status;
}

bool terminal::has_exited() {
  if (state_->child_pid <= 0)
    return true;
  int status{};
  const pid_t r = waitpid(state_->child_pid, &status, WNOHANG);
  if (r == 0)
    return false;
  state_->child_pid = -1;
  return true;
}

void terminal::stdio_pump_loop() {
  char buf[4096];
  for (;;) {
    const ssize_t n = read(state_->stdio_pipe_read_fd, buf, sizeof(buf));
    if (n <= 0)
      break; // EOF (write end closed in the destructor) or error

    const std::string translated = to_crlf(std::string_view(buf, static_cast<size_t>(n)));
    size_t written = 0;
    while (written < translated.size()) {
      const ssize_t w = write(state_->saved_stdout_fd, translated.data() + written, translated.size() - written);
      if (w < 0) {
        if (errno == EINTR)
          continue;
        return;
      }
      if (w == 0)
        return;
      written += static_cast<size_t>(w);
    }
  }
}

void terminal::pump_loop() {
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
      break;

    if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if (n <= 0)
        break;
      ssize_t written = 0;
      while (written < n) {
        const ssize_t w = write(write_handle_, buf + written, static_cast<size_t>(n - written));
        if (w <= 0)
          break;
        written += w;
      }
    }
  }
}

} // namespace bdg::bison::term
