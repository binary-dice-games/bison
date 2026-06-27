// MIT License © 2025 Binary Dice Games
/**
 * @file pty_client_transport.cpp
 * @brief PTY client transport implementation.
 *
 * Uses the process's own stdin/stdout (PTY slave or SSH channel) as a DCS
 * bison transport.  The client initiates the handshake: it emits HELLO first,
 * then waits for the server's HELLO response.
 */
#include "src/rmi/transport/pty_client_transport.hpp"

#if defined(__linux__)

#include "src/rmi/transport/dcs_framing.hpp"

#include <poll.h>
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

namespace bdg::bison::app {

namespace {

using namespace rmi::transport::dcs;

// ── Client transport state ────────────────────────────────────────────────────

struct client_state {
  int read_fd  = STDIN_FILENO;
  int write_fd = STDOUT_FILENO;

  std::mutex              write_mtx;
  std::mutex              read_mtx;
  std::condition_variable read_cv;

  std::queue<bison::buffer>                     inbox;
  std::unordered_map<uint64_t, partial_message> pending;

  std::atomic<bool>     closed{false};
  std::atomic<bool>     stop_requested{false};
  std::atomic<bool>     hello_seen{false};
  std::atomic<uint64_t> next_msg_id{1};

  size_t max_chunk_bytes = 1536U;
  size_t max_frame_bytes = 8U * 1024U * 1024U;
  std::chrono::milliseconds reassembly_timeout{5000};
  std::chrono::milliseconds handshake_timeout{300000};
};

// ── Reader thread ─────────────────────────────────────────────────────────────

/**
 * @brief Parse the stdin DCS stream and dispatch frames to the inbox.
 *
 * Non-DCS bytes on stdin are silently discarded — the client is not a
 * terminal emulator.  Exits when `stop_requested` is set or stdin reaches EOF.
 */
void client_reader_loop(std::shared_ptr<client_state> state) {
  if (!state)
    return;

  dcs_byte_parser parser{
      [&](const std::string& body) {
        process_body(*state, body);
      }
      // on_plain: empty — discard non-DCS bytes silently
  };

  while (!state->stop_requested.load()) {
    pollfd pfd{};
    pfd.fd     = state->read_fd;
    pfd.events = POLLIN;
    const int rc = ::poll(&pfd, 1, 100);
    if (rc == 0) continue;
    if (rc < 0) {
      if (errno == EINTR) continue;
      close_and_notify(*state);
      return;
    }
    if ((pfd.revents & POLLERR) != 0) {
      close_and_notify(*state);
      return;
    }

    uint8_t c = 0;
    const ssize_t n = ::read(state->read_fd, &c, 1);
    if (n == 0 || (n < 0 && errno != EINTR)) {
      close_and_notify(*state);
      return;
    }
    if (n < 0) continue; // EINTR

    parser.feed(c);
  }

  close_and_notify(*state);
}

} // namespace

// ── impl ──────────────────────────────────────────────────────────────────────

struct pty_client_transport::impl {
  impl() : state(std::make_shared<client_state>()) {}

  std::shared_ptr<client_state> state;
  std::thread                   reader;
  bool                          opened          = false;
  bool                          reader_detached = false;
  bool                          tty_active      = false;
  termios                       saved_tty{};
};

// ── pty_client_transport ──────────────────────────────────────────────────────

pty_client_transport::pty_client_transport()
    : impl_(std::make_unique<impl>()) {}

pty_client_transport::pty_client_transport(
    pty_client_transport&&) noexcept = default;
pty_client_transport& pty_client_transport::operator=(
    pty_client_transport&&) noexcept = default;

pty_client_transport::~pty_client_transport() {
  shutdown();
}

void pty_client_transport::open(bison::dynamic params) {
  if (!impl_ || !impl_->state)
    throw std::runtime_error("pty_client_transport::open: invalid state");
  if (impl_->reader_detached)
    throw std::runtime_error(
        "pty_client_transport::open: cannot reopen after shutdown");

  auto& st = *impl_->state;
  st.closed.store(false);
  st.stop_requested.store(false);
  st.hello_seen.store(false);

  if (const auto* f = params.findField("handshake_timeout_ms"_key);
      f != nullptr && f->is<int32_t>())
    st.handshake_timeout =
        std::chrono::milliseconds{std::max(0, f->as<int32_t>())};

  // Raw/no-echo mode so DCS frames are delivered immediately and not echoed.
  if (::isatty(STDIN_FILENO)) {
    termios tty{};
    if (::tcgetattr(STDIN_FILENO, &tty) == 0) {
      impl_->saved_tty   = tty;
      impl_->tty_active  = true;
      tty.c_lflag &= static_cast<tcflag_t>(
          ~(ICANON | ECHO | ECHOE | ECHOK | ECHONL | ISIG | ECHOCTL));
      tty.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | BRKINT));
      tty.c_cc[VMIN]  = 1;
      tty.c_cc[VTIME] = 0;
      (void)::tcsetattr(STDIN_FILENO, TCSANOW, &tty);
    }
  }

  // Start reader before emitting HELLO so the server's response is not missed.
  if (!impl_->reader.joinable())
    impl_->reader = std::thread(client_reader_loop, impl_->state);

  emit_dcs(st.write_fd, st,
      std::string{rmi::transport::dcs::kProtoVersion} + ";type=HELLO");

  {
    std::unique_lock<std::mutex> lk(st.read_mtx);
    const bool ok = st.read_cv.wait_for(lk, st.handshake_timeout, [&st] {
      return st.hello_seen.load() || st.closed.load();
    });
    if (!ok || !st.hello_seen.load())
      throw std::runtime_error(
          "pty_client_transport::open: handshake timeout");
  }

  impl_->opened = true;
}

void pty_client_transport::send(bison::buffer frame) {
  if (!impl_ || !impl_->state || !impl_->opened)
    throw std::runtime_error("pty_client_transport::send: not open");
  if (impl_->state->closed.load())
    throw std::runtime_error("pty_client_transport::send: closed");
  emit_data(impl_->state->write_fd, *impl_->state, frame);
}

bool pty_client_transport::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->state || !impl_->opened)
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

void pty_client_transport::shutdown() {
  if (!impl_ || !impl_->state)
    return;
  auto& st = *impl_->state;

  if (impl_->opened && !st.closed.load()) {
    try {
      emit_dcs(st.write_fd, st,
          std::string{rmi::transport::dcs::kProtoVersion} + ";type=END");
    } catch (...) {}
  }

  st.stop_requested.store(true);
  st.closed.store(true);
  st.read_cv.notify_all();

  if (impl_->reader.joinable()) {
    impl_->reader.detach();
    impl_->reader_detached = true;
  }

  if (impl_->tty_active) {
    (void)::tcsetattr(STDIN_FILENO, TCSANOW, &impl_->saved_tty);
    impl_->tty_active = false;
  }

  impl_->opened = false;
}

} // namespace bdg::bison::app

#endif // defined(__linux__)
