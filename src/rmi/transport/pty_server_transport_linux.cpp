// MIT License © 2025 Binary Dice Games
/**
 * @file pty_server_transport.cpp
 * @brief PTY-owning server transport implementation.
 *
 * Multiplexes a bison RMI channel and interactive terminal I/O over the same
 * PTY master fd.  DCS frames carry bison messages; plaintext bytes are relayed
 * to stdout so the user sees the shell normally.
 */
#include "src/rmi/transport/pty_server_transport.hpp"

#if defined(__linux__)

#include "src/rmi/transport/dcs_framing.hpp"

#include <poll.h>
#include <pty.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

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
 *        `pty_server_connection`.
 *
 * ### Locking rules
 * - `write_mtx`  — held for every write to `master_fd`; shared between the
 *                   input-relay thread and `pty_server_connection::send()`.
 * - `read_mtx`   — protects `inbox` and `pending`; transitions notify `read_cv`.
 * - `closed`, `hello_seen`, `shell_running`, `stop_requested` — atomics,
 *   readable from any thread without holding a mutex.
 * - All other fields are write-once before threads start and treated as
 *   read-only thereafter.
 */
struct pty_shared_state {
  // ── I/O ───────────────────────────────────────────────────────────────────
  int master_fd = -1;

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
 * @brief Background thread: read from PTY master and demultiplex the stream.
 *
 * Plaintext bytes are written to `stdout` (4 KiB buffer); DCS frames are
 * dispatched as bison messages via process_body().  Exits when
 * `stop_requested` is set or the PTY reaches EOF.
 */
void reader_loop(std::shared_ptr<pty_shared_state> state) {
  if (!state || state->master_fd < 0)
    return;

  std::vector<uint8_t> plain_buf;
  plain_buf.reserve(4096);

  const auto flush_plain = [&]() {
    if (!plain_buf.empty()) {
      (void)write_all_fd(STDOUT_FILENO, plain_buf.data(), plain_buf.size());
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
    pollfd pfd{};
    pfd.fd     = state->master_fd;
    pfd.events = POLLIN;
    const int rc = ::poll(&pfd, 1, 10);
    if (rc == 0) {
      flush_plain();
      continue;
    }
    if (rc < 0) {
      if (errno == EINTR) continue;
      flush_plain();
      state->shell_running.store(false);
      close_and_notify(*state);
      return;
    }
    if ((pfd.revents & POLLERR) != 0) {
      flush_plain();
      state->shell_running.store(false);
      close_and_notify(*state);
      return;
    }

    uint8_t c = 0;
    const ssize_t n = ::read(state->master_fd, &c, 1);
    if (n == 0 || (n < 0 && errno != EINTR)) {
      flush_plain();
      state->shell_running.store(false);
      close_and_notify(*state);
      return;
    }
    if (n < 0) continue; // EINTR

    parser.feed(c);
  }

  flush_plain();
  close_and_notify(*state);
}

// ── Input relay thread ────────────────────────────────────────────────────────

/**
 * @brief Background thread: relay user keystrokes from `stdin` to the PTY.
 *
 * Polls `stdin` with a 100 ms timeout so it can exit promptly when
 * `stop_requested` is set.  All writes to `master_fd` go through `write_mtx`
 * to serialise with outgoing bison frames.
 */
void input_relay_loop(std::shared_ptr<pty_shared_state> state) {
  if (!state)
    return;

  char buf[256];
  while (!state->stop_requested.load()) {
    pollfd pfd{};
    pfd.fd     = STDIN_FILENO;
    pfd.events = POLLIN;

    const int rc = ::poll(&pfd, 1, 100);
    if (rc == 0) continue;
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) break;
    if ((pfd.revents & (POLLIN | POLLHUP)) == 0) continue;

    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
    if (n > 0) {
      std::lock_guard<std::mutex> lk(state->write_mtx);
      if (state->master_fd >= 0)
        (void)write_all_fd(state->master_fd, buf, static_cast<size_t>(n));
    } else if (n == 0 || (n < 0 && errno != EINTR)) {
      break;
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

  std::string                       shell_;
  pid_t                             shell_pid_{-1};
  std::thread                       reader_thread_;
  std::thread                       input_relay_thread_;
  std::shared_ptr<pty_shared_state> state_;
  std::atomic<bool>                 started_{false};
  std::atomic<bool>                 accepted_{false};
  bool                              tty_active_{false};
  termios                           saved_tty_{};
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
  emit_data(impl_->state->master_fd, *impl_->state, frame);
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
  (void)params; // mode=dcs is forced; no other params apply

  if (impl_->started_.exchange(true))
    return; // idempotent

  int master_fd = -1;
  winsize ws{};
  ws.ws_col = 500;
  ws.ws_row = 50;
  const pid_t pid = ::forkpty(&master_fd, nullptr, nullptr, &ws);
  if (pid < 0) {
    impl_->started_.store(false);
    throw std::runtime_error("pty_server_transport::start: forkpty failed");
  }

  if (pid == 0) {
    ::execlp(impl_->shell_.c_str(), impl_->shell_.c_str(), nullptr);
    _exit(127);
  }

  impl_->shell_pid_          = pid;
  impl_->state_->master_fd   = master_fd;
  impl_->state_->shell_running.store(true);
  impl_->state_->stop_requested.store(false);

  if (::isatty(STDIN_FILENO)) {
    termios tty{};
    if (::tcgetattr(STDIN_FILENO, &tty) == 0) {
      impl_->saved_tty_ = tty;
      tty.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
      tty.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
      tty.c_cc[VMIN]  = 1;
      tty.c_cc[VTIME] = 0;
      if (::tcsetattr(STDIN_FILENO, TCSANOW, &tty) == 0)
        impl_->tty_active_ = true;
    }
  }

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

  emit_dcs(st.master_fd, st,
      std::string{rmi::transport::dcs::kProtoVersion} + ";type=HELLO");

  return std::make_unique<pty_server_connection>(
      std::make_unique<pty_server_connection::impl>(impl_->state_));
}

void pty_server_transport::stop() {
  if (!impl_->started_.load())
    return;

  auto& st = *impl_->state_;

  if (st.master_fd >= 0) {
    std::lock_guard<std::mutex> lk(st.write_mtx);
    const char exit_cmd[] = "exit\n";
    (void)write_all_fd(st.master_fd, exit_cmd, sizeof(exit_cmd) - 1);
  }

  st.stop_requested.store(true);
  st.shell_running.store(false);
  close_and_notify(st);

  if (st.master_fd >= 0) {
    ::close(st.master_fd);
    st.master_fd = -1;
  }

  if (impl_->tty_active_) {
    (void)::tcsetattr(STDIN_FILENO, TCSANOW, &impl_->saved_tty_);
    impl_->tty_active_ = false;
  }

  if (impl_->reader_thread_.joinable())
    impl_->reader_thread_.detach();
  if (impl_->input_relay_thread_.joinable())
    impl_->input_relay_thread_.detach();

  if (impl_->shell_pid_ > 0) {
    int status = 0;
    (void)::waitpid(impl_->shell_pid_, &status, 0);
    impl_->shell_pid_ = -1;
  }
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

#endif // defined(__linux__)
