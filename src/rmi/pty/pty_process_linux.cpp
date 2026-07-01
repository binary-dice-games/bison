// MIT License © 2025 Binary Dice Games
/**
 * @file pty_process_linux.cpp
 * @brief Linux PTY process implementation using forkpty(3).
 */
#ifdef __linux__

#include "src/rmi/pty/pty_process.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

// forkpty lives in <pty.h> on Linux glibc; needs -lutil.
#include <pty.h>

namespace bdg {
namespace bison {
namespace rmi {
namespace pty {

struct pty_process::impl {
  int master_fd{-1};
  pid_t child_pid{-1};
};

pty_process::pty_process(pty_config cfg) : impl_(std::make_unique<impl>()) {
  struct winsize ws{};
  ws.ws_col = cfg.cols;
  ws.ws_row = cfg.rows;

  const pid_t pid = forkpty(&impl_->master_fd, nullptr, nullptr, &ws);
  if (pid < 0)
    throw std::runtime_error("pty_process: forkpty failed");

  if (pid == 0) {
    // ── Child process ──────────────────────────────────────────────────────
    // Build argv: cmd followed by args.
    std::vector<char*> argv;
    argv.reserve(cfg.args.size() + 2);
    argv.push_back(const_cast<char*>(cfg.cmd.c_str()));
    for (auto& a : cfg.args)
      argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    execvp(cfg.cmd.c_str(), argv.data());
    // exec failed — _exit without running atexit handlers.
    _exit(127);
  }

  impl_->child_pid = pid;

  // Set master to non-blocking for reads (transport uses poll with timeout).
  const int flags = fcntl(impl_->master_fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl(impl_->master_fd, F_SETFL, flags & ~O_NONBLOCK);
}

pty_process::~pty_process() {
  if (!impl_)
    return;
  if (impl_->master_fd >= 0) {
    ::close(impl_->master_fd);
    impl_->master_fd = -1;
  }
}

pty_process::pty_process(pty_process&&) noexcept = default;
pty_process& pty_process::operator=(pty_process&&) noexcept = default;

int pty_process::master_fd() const noexcept {
  return impl_ ? impl_->master_fd : -1;
}

int pty_process::release_master_fd() noexcept {
  if (!impl_)
    return -1;
  const int fd = impl_->master_fd;
  impl_->master_fd = -1;
  return fd;
}

int pty_process::wait() {
  if (!impl_ || impl_->child_pid < 0)
    return -1;
  int status = 0;
  while (waitpid(impl_->child_pid, &status, 0) < 0) {
    if (errno != EINTR)
      return -1;
  }
  impl_->child_pid = -1;
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return -(int)WTERMSIG(status);
  return -1;
}

void pty_process::terminate() {
  if (impl_ && impl_->child_pid > 0)
    ::kill(impl_->child_pid, SIGTERM);
}

} // namespace pty
} // namespace rmi
} // namespace bison
} // namespace bdg

#endif // __linux__
