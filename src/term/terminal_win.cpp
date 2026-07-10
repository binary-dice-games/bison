// MIT License © 2025 Binary Dice Games
/**
 * @file terminal_win.cpp
 * @brief Native Windows implementation of terminal using ConPTY
 *        (`CreatePseudoConsole()`).
 *
 * Mirrors terminal_posix.cpp's forkpty()-based approach, but ConPTY's shape
 * differs from a POSIX pty in one structural way that matters to callers:
 * a pty master fd is a single bidirectional fd, while ConPTY hands back two
 * independent, unidirectional pipes (one for the child's output, one for its
 * input). `read_handle()`/`write_handle()` on this platform are therefore
 * two distinct fds, not the same one twice — term_transport already
 * supports that shape (its constructor takes read/write separately).
 */
#include "src/term/terminal.hpp"

#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace bdg::bison::term {

struct terminal_state {
  HPCON hpc{nullptr};
  PROCESS_INFORMATION pi{};
  LPPROC_THREAD_ATTRIBUTE_LIST attr_list{nullptr};
  std::vector<uint8_t> attr_list_buf;

  HANDLE conpty_in_write{nullptr}; // our end; write_handle_ wraps this
  HANDLE conpty_out_read{nullptr}; // our end; read_handle_ wraps this

  HANDLE console_in{nullptr};
  DWORD saved_console_mode{0};
  bool console_mode_saved{false};

  HANDLE stop_event{nullptr};

  // stdout/stderr redirect (see terminal.hpp's class doc comment).
  int saved_stdout_fd{-1};
  int stdio_pipe_read_fd{-1};
  int stdio_pipe_write_fd{-1};
  std::thread stdio_pump_thread;
};

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

/** @brief Best-effort child window size; falls back to 80x24 if the real console can't report one. */
COORD query_console_size() {
  COORD size{80, 24};
  const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (out != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(out, &info) != 0) {
    size.X = info.srWindow.Right - info.srWindow.Left + 1;
    size.Y = info.srWindow.Bottom - info.srWindow.Top + 1;
  }
  return size;
}

} // namespace

terminal::terminal(const std::string& cmd, const std::string& prompt_label)
    : state_(std::make_unique<terminal_state>()) {
  if (!prompt_label.empty()) {
    // cmd.exe has no rc file and reads PROMPT directly, so a plain env var
    // set in the parent -- inherited by CreateProcessA below via its
    // lpEnvironment == nullptr -- takes effect unconditionally. $P expands
    // to the current directory, $G to ">", matching the label:cwd$ layout
    // used by the POSIX PS1 in terminal_posix.cpp.
    _putenv_s("PROMPT", (prompt_label + " $P$G").c_str());
  }

  HANDLE pty_in_read = nullptr; // ConPTY's end: reads the child's stdin
  HANDLE pty_out_write = nullptr; // ConPTY's end: writes the child's stdout

  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);

  if (CreatePipe(&pty_in_read, &state_->conpty_in_write, nullptr, 0) == 0)
    throw std::runtime_error("terminal: CreatePipe (input) failed");
  if (CreatePipe(&state_->conpty_out_read, &pty_out_write, nullptr, 0) == 0)
    throw std::runtime_error("terminal: CreatePipe (output) failed");

  const HRESULT hr = CreatePseudoConsole(query_console_size(), pty_in_read, pty_out_write, 0, &state_->hpc);
  // ConPTY duplicates the handles it needs; our copies of its ends are no
  // longer needed once it's created, regardless of success or failure.
  CloseHandle(pty_in_read);
  CloseHandle(pty_out_write);
  if (FAILED(hr))
    throw std::runtime_error("terminal: CreatePseudoConsole failed");

  SIZE_T attr_list_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_list_size);
  state_->attr_list_buf.resize(attr_list_size);
  state_->attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(state_->attr_list_buf.data());
  if (InitializeProcThreadAttributeList(state_->attr_list, 1, 0, &attr_list_size) == 0)
    throw std::runtime_error("terminal: InitializeProcThreadAttributeList failed");
  if (UpdateProcThreadAttribute(
          state_->attr_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, state_->hpc, sizeof(HPCON), nullptr, nullptr) ==
      0) {
    DeleteProcThreadAttributeList(state_->attr_list);
    throw std::runtime_error("terminal: UpdateProcThreadAttribute failed");
  }

  std::string shell = cmd;
  if (shell.empty())
    shell = "cmd.exe";

  STARTUPINFOEXA startup{};
  startup.StartupInfo.cb = sizeof(STARTUPINFOEXA);
  startup.lpAttributeList = state_->attr_list;

  std::vector<char> cmdline(shell.begin(), shell.end());
  cmdline.push_back('\0');

  if (CreateProcessA(
          nullptr,
          cmdline.data(),
          nullptr,
          nullptr,
          FALSE,
          EXTENDED_STARTUPINFO_PRESENT,
          nullptr,
          nullptr,
          &startup.StartupInfo,
          &state_->pi) == 0) {
    DeleteProcThreadAttributeList(state_->attr_list);
    throw std::runtime_error("terminal: CreateProcess failed");
  }

  read_handle_ = _open_osfhandle(reinterpret_cast<intptr_t>(state_->conpty_out_read), _O_RDONLY);
  write_handle_ = _open_osfhandle(reinterpret_cast<intptr_t>(state_->conpty_in_write), _O_WRONLY);
  if (read_handle_ < 0 || write_handle_ < 0)
    throw std::runtime_error("terminal: _open_osfhandle failed");

  // Raw mode on our own real console input (not ConPTY's side): switch to
  // VT input mode so ReadFile() in pump_loop() delivers keystrokes
  // (including escape sequences for special keys) as raw bytes instead of
  // being intercepted by the console's line-input/echo/Ctrl-C handling —
  // the Windows equivalent of terminal_posix.cpp's cfmakeraw().
  state_->console_in = GetStdHandle(STD_INPUT_HANDLE);
  if (state_->console_in != INVALID_HANDLE_VALUE && GetConsoleMode(state_->console_in, &state_->saved_console_mode)) {
    state_->console_mode_saved = true;
    const DWORD raw_mode = ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT;
    SetConsoleMode(state_->console_in, raw_mode);
  }

  state_->stop_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);

  // Redirect fd 1/2 through a pipe + background byte-level CRLF translator
  // for the object's lifetime (see terminal.hpp's class doc comment).
  state_->saved_stdout_fd = _dup(_fileno(stdout));
  int stdio_pipe_fds[2];
  if (state_->saved_stdout_fd >= 0 && _pipe(stdio_pipe_fds, 4096, _O_BINARY) == 0) {
    state_->stdio_pipe_read_fd = stdio_pipe_fds[0];
    state_->stdio_pipe_write_fd = stdio_pipe_fds[1];
    fflush(stdout);
    fflush(stderr);
    _dup2(state_->stdio_pipe_write_fd, _fileno(stdout));
    _dup2(state_->stdio_pipe_write_fd, _fileno(stderr));
    state_->stdio_pump_thread = std::thread([this] { stdio_pump_loop(); });
  }
}

terminal::~terminal() {
  pump_running_.store(false);
  if (state_->stop_event != nullptr)
    SetEvent(state_->stop_event);
  if (pump_thread_.joinable())
    pump_thread_.join();

  if (state_->stdio_pipe_write_fd >= 0) {
    fflush(stdout);
    fflush(stderr);
    if (state_->saved_stdout_fd >= 0) {
      _dup2(state_->saved_stdout_fd, _fileno(stdout));
      _dup2(state_->saved_stdout_fd, _fileno(stderr));
    }
    _close(state_->stdio_pipe_write_fd); // last writer closed -> pump thread sees EOF
    if (state_->stdio_pump_thread.joinable())
      state_->stdio_pump_thread.join();
    _close(state_->stdio_pipe_read_fd);
    if (state_->saved_stdout_fd >= 0)
      _close(state_->saved_stdout_fd);
  }

  if (state_->console_mode_saved)
    SetConsoleMode(state_->console_in, state_->saved_console_mode);

  if (state_->hpc != nullptr)
    ClosePseudoConsole(state_->hpc); // also signals EOF to read_handle_

  if (state_->pi.hProcess != nullptr) {
    WaitForSingleObject(state_->pi.hProcess, INFINITE);
    CloseHandle(state_->pi.hProcess);
  }
  if (state_->pi.hThread != nullptr)
    CloseHandle(state_->pi.hThread);

  if (state_->attr_list != nullptr)
    DeleteProcThreadAttributeList(state_->attr_list);

  if (read_handle_ >= 0)
    _close(read_handle_);
  if (write_handle_ >= 0)
    _close(write_handle_);

  if (state_->stop_event != nullptr)
    CloseHandle(state_->stop_event);
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
  if (state_->pi.hProcess == nullptr)
    return -1;
  WaitForSingleObject(state_->pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(state_->pi.hProcess, &code);
  return static_cast<int>(code);
}

bool terminal::has_exited() {
  if (state_->pi.hProcess == nullptr)
    return true;
  return WaitForSingleObject(state_->pi.hProcess, 0) == WAIT_OBJECT_0;
}

void terminal::stdio_pump_loop() {
  char buf[4096];
  for (;;) {
    const int n = _read(state_->stdio_pipe_read_fd, buf, sizeof(buf));
    if (n <= 0)
      break; // EOF (write end closed in the destructor) or error

    const std::string translated = to_crlf(std::string_view(buf, static_cast<size_t>(n)));
    size_t written = 0;
    while (written < translated.size()) {
      const int w = _write(
          state_->saved_stdout_fd, translated.data() + written, static_cast<unsigned>(translated.size() - written));
      if (w <= 0)
        return;
      written += static_cast<size_t>(w);
    }
  }
}

void terminal::pump_loop() {
  char buf[4096];
  HANDLE waitables[2] = {state_->console_in, state_->stop_event};

  while (pump_running_.load()) {
    const DWORD wait_result = WaitForMultipleObjects(2, waitables, FALSE, INFINITE);
    if (wait_result == WAIT_OBJECT_0 + 1)
      break; // stop signaled

    if (wait_result != WAIT_OBJECT_0)
      break;

    DWORD read = 0;
    if (ReadFile(state_->console_in, buf, sizeof(buf), &read, nullptr) == 0 || read == 0)
      break;

    DWORD written_total = 0;
    while (written_total < read) {
      DWORD written = 0;
      if (WriteFile(
              reinterpret_cast<HANDLE>(_get_osfhandle(write_handle_)),
              buf + written_total,
              read - written_total,
              &written,
              nullptr) == 0 ||
          written == 0)
        break;
      written_total += written;
    }
  }
}

} // namespace bdg::bison::term
