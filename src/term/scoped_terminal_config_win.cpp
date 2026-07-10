// MIT License © 2025 Binary Dice Games
/**
 * @file scoped_terminal_config_win.cpp
 * @brief Native Windows half of scoped_terminal_config: console raw mode
 *        (`GetConsoleMode`/`SetConsoleMode`) plus the fd-level pass-through
 *        pumps (mirrors terminal_win.cpp's stdout/stderr redirect, applied
 *        to both directions here).
 */
#include "src/term/scoped_terminal_config.hpp"

#include "src/bison/bison_sync.hpp"

#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <string>
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

/** @brief Writes all of @p data to @p fd. @return `false` on error or EOF. */
bool write_all(int fd, std::string_view data) {
  size_t written = 0;
  while (written < data.size()) {
    const int w = _write(fd, data.data() + written, static_cast<unsigned>(data.size() - written));
    if (w <= 0)
      return false;
    written += static_cast<size_t>(w);
  }
  return true;
}

} // namespace

struct scoped_terminal_config::impl {
  // Raw mode, only meaningful if read_fd was a console.
  HANDLE console_handle{INVALID_HANDLE_VALUE};
  DWORD saved_console_mode{0};
  bool raw_mode_set{false};

  // Input redirection: the real fd read_fd used to refer to (dup'd aside),
  // and the write end of the pipe now installed in read_fd's place.
  int real_read_fd{-1};
  int input_pipe_write_fd{-1};
  bool input_pipe_write_closed{false};
  bool passthrough_last_was_cr{false}; // on_passthrough()'s CRLF-pair-splitting state
  std::string passthrough_line_buffer; // on_passthrough()'s not-yet-delivered line

  // Output redirection: the real fd write_fd used to refer to (dup'd
  // aside), and the pipe (whose write end is dup2'd onto write_fd and
  // stderr) drained by a background pump thread.
  int real_write_fd{-1};
  int output_pipe_read_fd{-1};
  int output_pipe_write_fd{-1};
  std::thread output_pump_thread;
  bison::synchronized<std::function<void(std::string_view)>> output_channel;
};

void scoped_terminal_config::run_output_pump(impl* state) {
  char buf[4096];
  for (;;) {
    const int n = _read(state->output_pipe_read_fd, buf, sizeof(buf));
    if (n <= 0)
      break; // EOF (write end closed on teardown) or error

    const std::string translated = to_crlf(std::string_view(buf, static_cast<size_t>(n)));
    const std::function<void(std::string_view)> channel = *state->output_channel.rlock();
    if (channel) {
      channel(translated);
      continue;
    }
    if (!write_all(state->real_write_fd, translated))
      return;
  }
}

scoped_terminal_config::impl_ptr scoped_terminal_config::create_state(const params& p) {
  impl_ptr state(new impl(), [](impl* s) { delete s; });

  const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(p.read_fd));
  if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &state->saved_console_mode)) {
    state->console_handle = handle;
    state->raw_mode_set = true;
    const DWORD raw_mode =
        state->saved_console_mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    SetConsoleMode(handle, raw_mode);
  }

  // Splice a pipe into read_fd's place; on_passthrough() feeds it.
  state->real_read_fd = _dup(p.read_fd);
  int input_pipe_fds[2];
  if (state->real_read_fd >= 0 && _pipe(input_pipe_fds, 4096, _O_BINARY) == 0) {
    _dup2(input_pipe_fds[0], p.read_fd);
    _close(input_pipe_fds[0]);
    state->input_pipe_write_fd = input_pipe_fds[1];
  }

  // Splice a pipe into write_fd's (and stderr's) place, pumped in the
  // background; forwards to set_output_channel()'s sink once set, else
  // straight to real_write_fd.
  state->real_write_fd = _dup(p.write_fd);
  int output_pipe_fds[2];
  if (state->real_write_fd >= 0 && _pipe(output_pipe_fds, 4096, _O_BINARY) == 0) {
    state->output_pipe_read_fd = output_pipe_fds[0];
    state->output_pipe_write_fd = output_pipe_fds[1];
    fflush(stdout);
    fflush(stderr);
    _dup2(state->output_pipe_write_fd, p.write_fd);
    _dup2(state->output_pipe_write_fd, _fileno(stderr));
    impl* raw_state = state.get();
    state->output_pump_thread = std::thread([raw_state] { run_output_pump(raw_state); });
  }

  return state;
}

void scoped_terminal_config::stop_output_pump() {
  impl* state = impl_.get();
  if (!state || state->output_pipe_write_fd < 0)
    return; // already stopped, or never started (write_fd redirection failed)

  fflush(stdout);
  fflush(stderr);
  if (state->real_write_fd >= 0) {
    _dup2(state->real_write_fd, params_.write_fd);
    _dup2(state->real_write_fd, _fileno(stderr));
  }
  _close(state->output_pipe_write_fd); // last writer closed -> pump sees EOF
  state->output_pipe_write_fd = -1; // makes this method idempotent
  if (state->output_pump_thread.joinable())
    state->output_pump_thread.join();
  _close(state->output_pipe_read_fd);
  if (state->real_write_fd >= 0)
    _close(state->real_write_fd);
}

void scoped_terminal_config::release_state(impl_ptr state) {
  stop_output_pump();

  if (state->real_read_fd >= 0) {
    if (state->raw_mode_set)
      SetConsoleMode(state->console_handle, state->saved_console_mode);
    _dup2(state->real_read_fd, params_.read_fd);
    _close(state->real_read_fd);
  }
  if (state->input_pipe_write_fd >= 0 && !state->input_pipe_write_closed)
    _close(state->input_pipe_write_fd);
}

int scoped_terminal_config::upstream_read_fd() const {
  return impl_->real_read_fd;
}

int scoped_terminal_config::upstream_write_fd() const {
  return impl_->real_write_fd;
}

void scoped_terminal_config::on_passthrough(std::string_view chunk) {
  if (impl_->input_pipe_write_fd < 0)
    return;

  if (chunk.empty()) {
    if (!impl_->input_pipe_write_closed) {
      _close(impl_->input_pipe_write_fd);
      impl_->input_pipe_write_closed = true;
    }
    return;
  }

  std::string to_echo;
  std::string to_deliver;
  process_passthrough_chunk(chunk, impl_->passthrough_last_was_cr, impl_->passthrough_line_buffer, to_echo, to_deliver);

  // Echo through the normal output path (params_.write_fd), so it's
  // CRLF-translated and serialized with everything else the same way any
  // other program output is.
  if (!to_echo.empty())
    write_all(params_.write_fd, to_echo);
  if (!to_deliver.empty())
    write_all(impl_->input_pipe_write_fd, to_deliver);
}

void scoped_terminal_config::on_terminal_passthrough(int fd, std::string_view chunk) {
  if (!chunk.empty())
    write_all(fd, chunk);
}

void scoped_terminal_config::set_output_channel(std::function<void(std::string_view)> sink) {
  *impl_->output_channel.wlock() = std::move(sink);
}

} // namespace bdg::bison::term
