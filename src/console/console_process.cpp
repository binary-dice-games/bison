// MIT License © 2025 Binary Dice Games
/**
 * @file console_process.cpp
 * @brief libuv (`uv_spawn`) implementation of console_process.
 */
#include "src/console/console_process.hpp"

#include <uv.h>

#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <csignal>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace bdg::bison::console {

/** @brief Close @p h unless it is already closing. */
static void close_if_active(uv_handle_t* h) {
  if (!uv_is_closing(h))
    uv_close(h, nullptr);
}

struct console_process_state {
  uv_loop_t loop{};
  uv_process_t proc{};
  uv_pipe_t child_stdin{};
  uv_pipe_t child_stdout{};
  uv_async_t stop_async{};
  std::thread loop_thread;
  bison::synchronized<std::optional<int>> exit_code;

  static void on_exit(uv_process_t* proc, int64_t exit_status, int term_signal) {
    auto* st = static_cast<console_process_state*>(proc->data);
    const int code = term_signal != 0 ? 128 + term_signal : static_cast<int>(exit_status);
    st->exit_code.withWLock([&](auto& ec) { ec = code; });
    st->exit_code.notify_all();
    close_if_active(reinterpret_cast<uv_handle_t*>(proc));
  }

  static void on_stop(uv_async_t* h) {
    auto* st = static_cast<console_process_state*>(h->data);
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&st->proc)))
      uv_process_kill(&st->proc, SIGTERM);
    close_if_active(reinterpret_cast<uv_handle_t*>(&st->stop_async));
  }
};

console_process::console_process(const std::string& cmd) : state_(std::make_unique<console_process_state>()) {
  if (cmd.empty())
    throw std::runtime_error("console_process: cmd must not be empty");

  uv_loop_init(&state_->loop);
  uv_pipe_init(&state_->loop, &state_->child_stdin, 0);
  uv_pipe_init(&state_->loop, &state_->child_stdout, 0);

  std::array<char*, 4> argv{
      const_cast<char*>("/bin/sh"), const_cast<char*>("-c"), const_cast<char*>(cmd.c_str()), nullptr};

  uv_stdio_container_t stdio[3]{};
  stdio[0].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_READABLE_PIPE);
  stdio[0].data.stream = reinterpret_cast<uv_stream_t*>(&state_->child_stdin);
  stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
  stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(&state_->child_stdout);
  stdio[2].flags = UV_INHERIT_FD;
  stdio[2].data.fd = 2;

  uv_process_options_t options{};
  options.file = "/bin/sh";
  options.args = argv.data();
  options.stdio_count = 3;
  options.stdio = stdio;
  options.exit_cb = console_process_state::on_exit;

  state_->proc.data = state_.get();
  const int r = uv_spawn(&state_->loop, &state_->proc, &options);
  if (r != 0)
    throw std::runtime_error(std::string{"console_process: uv_spawn failed: "} + uv_strerror(r));

  // Extract the parent-side fds uv_spawn created, then hand the two pipe
  // handles off entirely — stdio_server_transport (via dup_stdio_fd) becomes
  // the sole reader/writer of the duplicated fds, preserving the
  // single-reader invariant documented in src/pty/DESIGN.md.
  uv_os_fd_t fd{};
  if (uv_fileno(reinterpret_cast<uv_handle_t*>(&state_->child_stdin), &fd) != 0)
    throw std::runtime_error("console_process: uv_fileno (stdin) failed");
  write_fd_ = dup(fd);

  if (uv_fileno(reinterpret_cast<uv_handle_t*>(&state_->child_stdout), &fd) != 0)
    throw std::runtime_error("console_process: uv_fileno (stdout) failed");
  read_fd_ = dup(fd);

  // libuv opens its pipe stdio containers in non-blocking mode; dup()
  // shares that flag with the fds handed off here. Clear it so read_fd()/
  // write_fd() behave like ordinary blocking fds (matching pty_process's
  // master_fd()) — stdio_server_transport re-forces non-blocking itself via
  // uv_pipe_open() regardless, so this only affects callers that read/write
  // these fds directly.
  fcntl(read_fd_, F_SETFL, fcntl(read_fd_, F_GETFL, 0) & ~O_NONBLOCK);
  fcntl(write_fd_, F_SETFL, fcntl(write_fd_, F_GETFL, 0) & ~O_NONBLOCK);

  uv_close(reinterpret_cast<uv_handle_t*>(&state_->child_stdin), nullptr);
  uv_close(reinterpret_cast<uv_handle_t*>(&state_->child_stdout), nullptr);

  uv_async_init(&state_->loop, &state_->stop_async, console_process_state::on_stop);
  state_->stop_async.data = state_.get();

  state_->loop_thread = std::thread([st = state_.get()] {
    uv_run(&st->loop, UV_RUN_DEFAULT);
    uv_loop_close(&st->loop);
  });
}

console_process::~console_process() {
  if (state_ && state_->loop_thread.joinable()) {
    uv_async_send(&state_->stop_async);
    state_->loop_thread.join();
  }
  if (read_fd_ >= 0)
    close(read_fd_);
  if (write_fd_ >= 0)
    close(write_fd_);
}

int console_process::read_fd() const {
  return read_fd_;
}

int console_process::write_fd() const {
  return write_fd_;
}

int console_process::wait() {
  int result = -1;
  state_->exit_code.wait([&](auto& ec) {
    if (ec.has_value()) {
      result = *ec;
      return true;
    }
    return false;
  });
  return result;
}

} // namespace bdg::bison::console
