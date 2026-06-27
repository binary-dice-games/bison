// MIT License © 2025 Binary Dice Games
/**
 * @file pty_server_transport_win.cpp
 * @brief Windows PTY server transport implementation using ConPTY.
 *
 * Mirrors pty_server_transport.cpp structurally.  Replaces the single Linux
 * `master_fd` (int) with two Windows anonymous-pipe HANDLEs backed by a
 * ConPTY pseudo-console.  All shared framing logic is inherited from
 * dcs_framing.hpp via template overload resolution on write_all_fd(HANDLE).
 *
 * Requires Windows 10 1809 / SDK 17763 (_WIN32_WINNT=0x0A00), set per-source
 * in CMake to avoid raising the global version used by ASIO.
 */
#include "src/rmi/transport/pty_server_transport.hpp"

#if defined(_WIN32)

#include "src/rmi/transport/dcs_framing.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bdg::bison::app {

namespace {

using namespace rmi::transport::dcs;

// ── Shared session state ──────────────────────────────────────────────────────

/**
 * @brief All mutable state shared between `pty_server_transport` and
 *        `pty_server_connection` on Windows.
 *
 * Replaces the single Linux `master_fd` with two pipe HANDLEs:
 * - `hRead`  — server reads shell/ConPTY output from this end.
 * - `hWrite` — server writes keystrokes / DCS frames into ConPTY.
 *
 * Locking rules mirror the Linux implementation.
 */
struct pty_shared_state {
  // ── I/O ───────────────────────────────────────────────────────────────────
  HANDLE hRead  = INVALID_HANDLE_VALUE; // server reads  ← ConPTY output pipe
  HANDLE hWrite = INVALID_HANDLE_VALUE; // server writes → ConPTY input pipe

  // ── Write lock ────────────────────────────────────────────────────────────
  std::mutex write_mtx;

  // ── Inbox (protected by read_mtx) ─────────────────────────────────────────
  std::mutex                                    read_mtx;
  std::condition_variable                       read_cv;
  std::queue<bison::buffer>                     inbox;
  std::unordered_map<uint64_t, partial_message> pending;

  // ── Session atomics (reset by restart_session()) ──────────────────────────
  std::atomic<bool> closed{false};
  std::atomic<bool> hello_seen{false};

  // ── Transport-level atomics ───────────────────────────────────────────────
  std::atomic<bool>     shell_running{false};
  std::atomic<bool>     stop_requested{false};
  std::atomic<uint64_t> next_msg_id{1};

  // ── Configuration (set once before threads start) ─────────────────────────
  size_t max_chunk_bytes = 1536U;
  size_t max_frame_bytes = 8U * 1024U * 1024U;
  std::chrono::milliseconds reassembly_timeout{5000};
};

// ── Reader thread ─────────────────────────────────────────────────────────────

/**
 * @brief Background thread: read from hRead and demultiplex the stream.
 *
 * Uses PeekNamedPipe (10 ms sleep when empty) instead of poll().
 * Plaintext bytes are forwarded to stdout; DCS frames are dispatched via
 * process_body().
 */
void reader_loop(std::shared_ptr<pty_shared_state> state) {
  if (!state || state->hRead == INVALID_HANDLE_VALUE)
    return;

  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);

  std::vector<uint8_t> plain_buf;
  plain_buf.reserve(4096);

  const auto flush_plain = [&]() {
    if (!plain_buf.empty()) {
      DWORD written = 0;
      (void)WriteFile(hOut, plain_buf.data(),
                      static_cast<DWORD>(plain_buf.size()), &written, nullptr);
      plain_buf.clear();
    }
  };

  dcs_byte_parser parser{
      [&](const std::string& body) {
        flush_plain();
        process_body(*state, body);
      },
      [&](uint8_t c) {
        plain_buf.push_back(c);
        if (plain_buf.size() >= 4096)
          flush_plain();
      }};

  while (!state->stop_requested.load()) {
    DWORD available = 0;
    if (!PeekNamedPipe(state->hRead, nullptr, 0, nullptr, &available,
                       nullptr)) {
      flush_plain();
      state->shell_running.store(false);
      close_and_notify(*state);
      return;
    }
    if (available == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
      continue;
    }

    uint8_t c = 0;
    DWORD n = 0;
    if (!ReadFile(state->hRead, &c, 1, &n, nullptr) || n == 0) {
      flush_plain();
      state->shell_running.store(false);
      close_and_notify(*state);
      return;
    }

    parser.feed(c);
  }

  flush_plain();
  close_and_notify(*state);
}

// ── Input relay thread ────────────────────────────────────────────────────────

/**
 * @brief Background thread: relay stdin keystrokes to the ConPTY input pipe.
 *
 * Uses WaitForSingleObject (100 ms timeout) for console handles, or
 * PeekNamedPipe (10 ms sleep) for pipe handles (e.g. redirected stdin).
 */
void input_relay_loop(std::shared_ptr<pty_shared_state> state) {
  if (!state)
    return;

  HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
  char buf[256];

  while (!state->stop_requested.load()) {
    if (GetFileType(hIn) == FILE_TYPE_CHAR) {
      // Console handle — use WaitForSingleObject for efficient blocking.
      if (WaitForSingleObject(hIn, 100) != WAIT_OBJECT_0)
        continue;
    } else {
      // Pipe / redirected stdin — use PeekNamedPipe.
      DWORD avail = 0;
      if (!PeekNamedPipe(hIn, nullptr, 0, nullptr, &avail, nullptr))
        break;
      if (avail == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        continue;
      }
    }

    DWORD n = 0;
    if (!ReadFile(hIn, buf, sizeof(buf), &n, nullptr) || n == 0)
      break;

    if (state->hWrite != INVALID_HANDLE_VALUE) {
      std::lock_guard<std::mutex> lk(state->write_mtx);
      write_all_fd(state->hWrite, buf, static_cast<size_t>(n));
    }
  }
}

} // namespace

// ── impl structs ──────────────────────────────────────────────────────────────

struct pty_server_connection::impl {
  explicit impl(std::shared_ptr<pty_shared_state> s) : state(std::move(s)) {}
  std::shared_ptr<pty_shared_state> state;
};

struct pty_server_transport::impl {
  explicit impl(std::string shell)
      : shell_(std::move(shell)),
        state_(std::make_shared<pty_shared_state>()) {}

  std::string shell_;
  HPCON       hPC_{nullptr};
  HANDLE      hShellProcess_{INVALID_HANDLE_VALUE};
  HANDLE      hShellThread_ {INVALID_HANDLE_VALUE};
  HANDLE      hPipeIn_ {INVALID_HANDLE_VALUE}; // server-side write → ConPTY
  HANDLE      hPipeOut_{INVALID_HANDLE_VALUE}; // server-side read  ← ConPTY
  std::shared_ptr<pty_shared_state> state_;
  std::thread reader_thread_;
  std::thread input_relay_thread_;
  std::atomic<bool> started_{false};
  std::atomic<bool> accepted_{false};
};

// ══════════════════════════════════════════════════════════════════════════════
// pty_server_connection
// ══════════════════════════════════════════════════════════════════════════════

pty_server_connection::pty_server_connection(std::unique_ptr<impl> impl)
    : impl_(std::move(impl)) {}

pty_server_connection::~pty_server_connection() {
  close();
}

pty_server_connection::pty_server_connection(
    pty_server_connection&&) noexcept = default;
pty_server_connection& pty_server_connection::operator=(
    pty_server_connection&&) noexcept = default;

void pty_server_connection::send(bison::buffer frame) {
  if (!impl_ || !impl_->state)
    throw std::runtime_error("pty_server_connection::send: no state");
  if (impl_->state->closed.load())
    throw std::runtime_error("pty_server_connection::send: closed");
  emit_data(impl_->state->hWrite, *impl_->state, frame);
}

bool pty_server_connection::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->state)
    return false;
  auto& st = *impl_->state;
  std::unique_lock<std::mutex> lk(st.read_mtx);
  if (!st.read_cv.wait_for(lk, timeout, [&st] {
        return !st.inbox.empty() || st.closed.load();
      }))
    return false;
  if (st.inbox.empty())
    return false;
  frame = std::move(st.inbox.front());
  st.inbox.pop();
  return true;
}

void pty_server_connection::close() {
  if (!impl_ || !impl_->state)
    return;
  close_and_notify(*impl_->state);
}

bool pty_server_connection::is_closed() const {
  return !impl_ || !impl_->state || impl_->state->closed.load();
}

// ══════════════════════════════════════════════════════════════════════════════
// pty_server_transport
// ══════════════════════════════════════════════════════════════════════════════

pty_server_transport::pty_server_transport(std::string shell)
    : impl_(std::make_unique<impl>(std::move(shell))) {}

pty_server_transport::~pty_server_transport() {
  stop();
}

void pty_server_transport::start(bison::dynamic params) {
  (void)params;

  if (impl_->started_.exchange(true))
    return; // idempotent

  // 1. Create pipe pairs — handles are non-inheritable in this process.
  HANDLE hInRead, hInWrite, hOutRead, hOutWrite;
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
  if (!CreatePipe(&hInRead, &hInWrite, &sa, 0) ||
      !CreatePipe(&hOutRead, &hOutWrite, &sa, 0)) {
    impl_->started_.store(false);
    throw std::runtime_error("pty_server_transport: CreatePipe failed");
  }

  // 2. Create the ConPTY, passing it the child-facing pipe ends.
  COORD size{500, 50};
  HRESULT hr = CreatePseudoConsole(size, hInRead, hOutWrite, 0, &impl_->hPC_);
  CloseHandle(hInRead);   // now owned by ConPTY
  CloseHandle(hOutWrite); // now owned by ConPTY
  if (FAILED(hr)) {
    CloseHandle(hInWrite);
    CloseHandle(hOutRead);
    impl_->started_.store(false);
    throw std::runtime_error("pty_server_transport: CreatePseudoConsole failed");
  }

  // 3. Retain the server-side pipe ends.
  impl_->hPipeIn_       = hInWrite;
  impl_->hPipeOut_      = hOutRead;
  impl_->state_->hWrite = hInWrite;
  impl_->state_->hRead  = hOutRead;

  // 4. Build STARTUPINFOEX with PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE.
  SIZE_T attrListSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attrListSize);
  std::vector<uint8_t> attrBuf(attrListSize);
  auto* pAttrList =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
  if (!InitializeProcThreadAttributeList(pAttrList, 1, 0, &attrListSize)) {
    ClosePseudoConsole(impl_->hPC_);
    CloseHandle(hInWrite);
    CloseHandle(hOutRead);
    impl_->started_.store(false);
    throw std::runtime_error(
        "pty_server_transport: InitializeProcThreadAttributeList failed");
  }
  UpdateProcThreadAttribute(pAttrList, 0,
      PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, impl_->hPC_, sizeof(impl_->hPC_),
      nullptr, nullptr);

  STARTUPINFOEXW si{};
  si.StartupInfo.cb  = sizeof(si);
  si.lpAttributeList = pAttrList;

  // 5. Launch the shell.  Widen the command — sufficient for cmd.exe / ASCII.
  std::wstring cmd(impl_->shell_.begin(), impl_->shell_.end());
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(
      nullptr, cmd.data(), nullptr, nullptr, FALSE,
      EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
      reinterpret_cast<LPSTARTUPINFOW>(&si), &pi);
  DeleteProcThreadAttributeList(pAttrList);
  if (!ok) {
    ClosePseudoConsole(impl_->hPC_);
    impl_->hPC_ = nullptr;
    CloseHandle(hInWrite);
    CloseHandle(hOutRead);
    impl_->started_.store(false);
    throw std::runtime_error("pty_server_transport: CreateProcess failed");
  }
  impl_->hShellProcess_ = pi.hProcess;
  impl_->hShellThread_  = pi.hThread;
  impl_->state_->shell_running.store(true);
  impl_->state_->stop_requested.store(false);

  // 6. Start reader and input-relay threads.
  impl_->reader_thread_      = std::thread(reader_loop,      impl_->state_);
  impl_->input_relay_thread_ = std::thread(input_relay_loop, impl_->state_);
}

std::unique_ptr<rmi::transport::server_connection_iface>
pty_server_transport::accept(std::chrono::milliseconds timeout) {
  auto& st = *impl_->state_;

  if (impl_->accepted_.exchange(true)) {
    std::unique_lock<std::mutex> lk(st.read_mtx);
    st.read_cv.wait_for(lk, timeout, [&st] {
      return st.closed.load() || !st.shell_running.load();
    });
    return nullptr;
  }

  {
    std::unique_lock<std::mutex> lk(st.read_mtx);
    const bool ok = st.read_cv.wait_for(lk, timeout, [&st] {
      return st.hello_seen.load() || !st.shell_running.load();
    });
    if (!ok || !st.hello_seen.load()) {
      impl_->accepted_.store(false);
      return nullptr;
    }
  }

  emit_dcs(st.hWrite, st,
      std::string{rmi::transport::dcs::kProtoVersion} + ";type=HELLO");

  return std::make_unique<pty_server_connection>(
      std::make_unique<pty_server_connection::impl>(impl_->state_));
}

void pty_server_transport::stop() {
  if (!impl_->started_.load())
    return;

  auto& st = *impl_->state_;

  // Signal the shell to exit gracefully.
  if (st.hWrite != INVALID_HANDLE_VALUE) {
    std::lock_guard<std::mutex> lk(st.write_mtx);
    const char exit_cmd[] = "exit\r\n";
    write_all_fd(st.hWrite, exit_cmd, sizeof(exit_cmd) - 1);
  }

  st.stop_requested.store(true);
  st.shell_running.store(false);
  close_and_notify(st);

  if (impl_->hPC_) {
    ClosePseudoConsole(impl_->hPC_);
    impl_->hPC_ = nullptr;
  }
  if (impl_->hPipeIn_ != INVALID_HANDLE_VALUE) {
    CloseHandle(impl_->hPipeIn_);
    impl_->hPipeIn_ = INVALID_HANDLE_VALUE;
  }
  if (impl_->hPipeOut_ != INVALID_HANDLE_VALUE) {
    CloseHandle(impl_->hPipeOut_);
    impl_->hPipeOut_ = INVALID_HANDLE_VALUE;
  }

  if (impl_->hShellProcess_ != INVALID_HANDLE_VALUE) {
    WaitForSingleObject(impl_->hShellProcess_, 5000);
    CloseHandle(impl_->hShellProcess_);
    impl_->hShellProcess_ = INVALID_HANDLE_VALUE;
    CloseHandle(impl_->hShellThread_);
    impl_->hShellThread_ = INVALID_HANDLE_VALUE;
  }

  if (impl_->reader_thread_.joinable())
    impl_->reader_thread_.detach();
  if (impl_->input_relay_thread_.joinable())
    impl_->input_relay_thread_.detach();
}

void pty_server_transport::restart_session() {
  auto& st = *impl_->state_;
  {
    std::lock_guard<std::mutex> lk(st.read_mtx);
    st.inbox   = std::queue<bison::buffer>{};
    st.pending.clear();
  }
  st.closed.store(false);
  st.hello_seen.store(false);
  impl_->accepted_.store(false);
}

bool pty_server_transport::is_shell_running() const {
  return impl_->state_->shell_running.load();
}

bool pty_server_transport::wait_until_closed(
    std::chrono::milliseconds timeout) const {
  auto& st = *impl_->state_;
  std::unique_lock<std::mutex> lk(st.read_mtx);
  return st.read_cv.wait_for(lk, timeout, [&st] {
    return st.closed.load() || !st.shell_running.load();
  });
}

} // namespace bdg::bison::app

#endif // defined(_WIN32)
