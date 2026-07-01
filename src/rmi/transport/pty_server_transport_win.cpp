// MIT License © 2025 Binary Dice Games
/**
 * @file pty_server_transport_win.cpp
 * @brief Windows implementation of pty_server_transport using ConPTY.
 *
 * Three background threads are launched per connection:
 *  - process_watcher_thread: blocks on WaitForMultipleObjects(h_process,
 *    h_stop_event) and fires close_and_notify + on_closed the instant the
 *    child exits. This is required because ConPTY output pipes do NOT close
 *    when the child exits — only when ClosePseudoConsole() is called — so the
 *    read thread cannot detect child exit via a broken pipe.
 *  - read_thread:         h_out_read → dcs_byte_parser → inbox / on_plain
 *  - stdin_relay_thread:  server stdin → h_in_write (forwards keystrokes)
 *
 * The server's console stdin is switched to raw VT input mode while the
 * connection is live (ENABLE_VIRTUAL_TERMINAL_INPUT, no echo, no line
 * buffering) so that keystrokes and special keys are forwarded correctly.
 * The original console mode is restored when the connection closes.
 */
#ifdef _WIN32

#include "src/rmi/transport/pty_server_transport.hpp"
#include "src/rmi/transport/pty_channel_state.hpp"
#include "src/rmi/transport/dcs_framing.hpp"

#include <stdexcept>

#include <windows.h>

namespace bdg {
namespace bison {
namespace rmi {
namespace transport {

// ── Per-connection impl ───────────────────────────────────────────────────────

struct pty_server_conn_impl {
  pty::pty_process process;
  pty_channel_state st;
  HANDLE h_out_read{INVALID_HANDLE_VALUE};
  HANDLE h_in_write{INVALID_HANDLE_VALUE};
  HANDLE h_process{INVALID_HANDLE_VALUE};  // non-owning; for watcher thread
  HANDLE h_stop_event{nullptr};            // manual-reset event; unblocks watcher
  std::function<void(uint8_t)> on_plain;
  std::function<void()> on_closed;
  std::thread process_watcher_thread;
  std::thread stdin_relay_thread;
  DWORD saved_stdin_mode{0};
  bool stdin_mode_saved{false};

  explicit pty_server_conn_impl(pty::pty_process p,
                                std::function<void(uint8_t)> plain_cb,
                                std::function<void()> closed_cb)
      : process(std::move(p)), on_plain(std::move(plain_cb)),
        on_closed(std::move(closed_cb)) {
    process.release_handles(h_out_read, h_in_write);
    h_process = process.h_process();
    // Manual-reset, initially unsignaled — set by close() to unblock the watcher.
    h_stop_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  }

  ~pty_server_conn_impl() {
    // Ensure all threads are stopped before handles are closed.
    st.stopped.store(true);
    if (h_stop_event)
      SetEvent(h_stop_event);
    if (process_watcher_thread.joinable())
      process_watcher_thread.join();
    if (stdin_relay_thread.joinable())
      stdin_relay_thread.join();
    // Join read_thread explicitly here, before h_out_read is closed below.
    // ~pty_channel_state() would join it too, but by then the handle is gone.
    if (st.read_thread.joinable())
      st.read_thread.join();
    if (stdin_mode_saved) {
      SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), saved_stdin_mode);
      stdin_mode_saved = false;
    }
    if (h_out_read != INVALID_HANDLE_VALUE) {
      CloseHandle(h_out_read);
      h_out_read = INVALID_HANDLE_VALUE;
    }
    if (h_in_write != INVALID_HANDLE_VALUE) {
      CloseHandle(h_in_write);
      h_in_write = INVALID_HANDLE_VALUE;
    }
    if (h_stop_event) {
      CloseHandle(h_stop_event);
      h_stop_event = nullptr;
    }
  }
};

// ── Background threads ────────────────────────────────────────────────────────

/**
 * Blocks until the child process exits (or the stop event fires), then
 * marks the channel closed and invokes on_closed.
 *
 * ConPTY output pipes stay open after the child exits and cannot signal EOF,
 * so polling the process handle directly is the only reliable exit detector.
 */
static void run_process_watcher_thread(HANDLE h_process, HANDLE h_stop_event,
                                       pty_channel_state& st,
                                       std::function<void()> on_closed) {
  if (h_process == INVALID_HANDLE_VALUE)
    return;

  HANDLE handles[2] = {h_process, h_stop_event};
  const DWORD count = h_stop_event ? 2 : 1;
  const DWORD result = WaitForMultipleObjects(count, handles, FALSE, INFINITE);

  if (result == WAIT_OBJECT_0) {
    // Child process exited — wake up the receive side and notify the server.
    dcs::close_and_notify(st);
    if (on_closed)
      on_closed();
  }
  // result == WAIT_OBJECT_0+1: stop event fired (close() called); just return.
}

static void run_read_thread(HANDLE h_out, pty_channel_state& st,
                            std::function<void(uint8_t)> on_plain) {
  dcs::dcs_byte_parser parser(
      [&st](const std::string& body) { dcs::process_body(st, body); },
      std::move(on_plain));

  BYTE buf[4096];
  while (!st.stopped.load()) {
    DWORD avail = 0;
    if (!PeekNamedPipe(h_out, nullptr, 0, nullptr, &avail, nullptr))
      break;
    if (avail == 0) {
      Sleep(10);
      continue;
    }
    DWORD n = 0;
    const DWORD to_read = avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf));
    if (!ReadFile(h_out, buf, to_read, &n, nullptr) || n == 0)
      break;
    for (DWORD i = 0; i < n; ++i)
      parser.feed(static_cast<uint8_t>(buf[i]));
  }
  // Wake any blocked dequeue() callers in case the pipe broke without the
  // process having exited yet (e.g. an explicit ClosePseudoConsole call).
  dcs::close_and_notify(st);
}

// Relay server stdin → ConPTY input so keystrokes reach the child process.
static void run_stdin_relay_thread(HANDLE h_in_write, pty_channel_state& st) {
  HANDLE h_stdin = GetStdHandle(STD_INPUT_HANDLE);
  BYTE buf[256];
  while (!st.stopped.load()) {
    // WaitForSingleObject on a console handle signals when input is available.
    DWORD wait = WaitForSingleObject(h_stdin, 100);
    if (wait == WAIT_FAILED)
      break;
    if (wait == WAIT_TIMEOUT)
      continue;
    DWORD n = 0;
    if (!ReadFile(h_stdin, buf, sizeof(buf), &n, nullptr) || n == 0)
      break;
    DWORD written = 0;
    if (!WriteFile(h_in_write, buf, n, &written, nullptr))
      break;
  }
}

// ── pty_server_connection ─────────────────────────────────────────────────────

pty_server_connection::pty_server_connection(
    std::unique_ptr<pty_server_conn_impl> impl)
    : impl_(std::move(impl)) {
  // Switch the server's console stdin to raw VT mode so individual keystrokes
  // and special keys (arrows, function keys) are forwarded as VT sequences.
  HANDLE h_stdin = GetStdHandle(STD_INPUT_HANDLE);
  if (GetConsoleMode(h_stdin, &impl_->saved_stdin_mode)) {
    impl_->stdin_mode_saved = true;
    DWORD mode = impl_->saved_stdin_mode;
    mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    SetConsoleMode(h_stdin, mode);
  }

  impl_->process_watcher_thread = std::thread(
      run_process_watcher_thread, impl_->h_process, impl_->h_stop_event,
      std::ref(impl_->st), impl_->on_closed);
  impl_->st.read_thread = std::thread(
      run_read_thread, impl_->h_out_read,
      std::ref(impl_->st), impl_->on_plain);
  impl_->stdin_relay_thread = std::thread(
      run_stdin_relay_thread, impl_->h_in_write, std::ref(impl_->st));
}

pty_server_connection::~pty_server_connection() {
  close();
}

void pty_server_connection::send(bison::buffer frame) {
  if (impl_->st.closed.load())
    throw std::runtime_error("pty_server_connection::send: connection closed");
  dcs::emit_data(impl_->h_in_write, impl_->st, frame);
}

bool pty_server_connection::receive(bison::buffer& frame,
                                    std::chrono::milliseconds timeout) {
  return impl_->st.dequeue(frame, timeout);
}

void pty_server_connection::close() {
  impl_->st.stopped.store(true);
  // Signal the watcher so it unblocks from WaitForMultipleObjects immediately.
  if (impl_->h_stop_event)
    SetEvent(impl_->h_stop_event);
  if (impl_->process_watcher_thread.joinable())
    impl_->process_watcher_thread.join();
  // stdin relay exits within one 100 ms poll timeout once stopped is set.
  if (impl_->stdin_relay_thread.joinable())
    impl_->stdin_relay_thread.join();
  // read_thread exits within one 10 ms Sleep once stopped is set.
  if (impl_->st.read_thread.joinable())
    impl_->st.read_thread.join();
  // Restore console stdin mode before signalling closed.
  if (impl_->stdin_mode_saved) {
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), impl_->saved_stdin_mode);
    impl_->stdin_mode_saved = false;
  }
  dcs::close_and_notify(impl_->st);
}

bool pty_server_connection::is_closed() const {
  return impl_->st.closed.load();
}

// ── pty_server_transport ──────────────────────────────────────────────────────

pty_server_transport::pty_server_transport(pty::pty_config cfg, plain_cb on_plain,
                                           closed_cb on_closed)
    : cfg_(std::move(cfg)), on_plain_(std::move(on_plain)),
      on_closed_(std::move(on_closed)) {}

pty_server_transport::~pty_server_transport() {
  stop();
}

void pty_server_transport::start(bison::dynamic /*params*/) {
  if (stopped_.load())
    return;
  process_ = std::make_unique<pty::pty_process>(cfg_);
}

std::unique_ptr<server_connection_iface> pty_server_transport::accept(
    std::chrono::milliseconds /*timeout*/) {
  if (stopped_.load() || !process_)
    return nullptr;
  if (accepted_.exchange(true))
    return nullptr;
  auto impl = std::make_unique<pty_server_conn_impl>(std::move(*process_),
                                                     on_plain_, on_closed_);
  process_.reset();
  return std::make_unique<pty_server_connection>(std::move(impl));
}

void pty_server_transport::stop() {
  stopped_.store(true);
  if (process_) {
    process_->terminate();
    process_->wait();
    process_.reset();
  }
}

} // namespace transport
} // namespace rmi
} // namespace bison
} // namespace bdg

#endif // _WIN32
