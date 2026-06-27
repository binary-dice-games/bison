// MIT License © 2025 Binary Dice Games
/**
 * @file pty_client_transport_win.cpp
 * @brief Windows PTY client transport implementation.
 *
 * Mirrors pty_client_transport.cpp structurally.  Uses GetStdHandle() to
 * obtain stdin/stdout as Windows HANDLEs instead of POSIX file descriptors.
 * No termios raw-mode block is needed — stdin is already a pipe inside the
 * ConPTY session.
 */
#include "src/rmi/transport/pty_client_transport.hpp"

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

namespace bdg::bison::app {

namespace {

using namespace rmi::transport::dcs;

// ── Client transport state ────────────────────────────────────────────────────

struct client_state {
  HANDLE read_h  = INVALID_HANDLE_VALUE; // STD_INPUT_HANDLE
  HANDLE write_h = INVALID_HANDLE_VALUE; // STD_OUTPUT_HANDLE

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
 * Uses the same PeekNamedPipe / WaitForSingleObject polling pattern as the
 * server's input_relay_loop.  Non-DCS bytes are silently discarded — the
 * client is not a terminal emulator.
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

  HANDLE hIn = state->read_h;

  while (!state->stop_requested.load()) {
    if (GetFileType(hIn) == FILE_TYPE_CHAR) {
      if (WaitForSingleObject(hIn, 100) != WAIT_OBJECT_0)
        continue;
    } else {
      DWORD avail = 0;
      if (!PeekNamedPipe(hIn, nullptr, 0, nullptr, &avail, nullptr)) {
        close_and_notify(*state);
        return;
      }
      if (avail == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        continue;
      }
    }

    uint8_t c = 0;
    DWORD n = 0;
    if (!ReadFile(hIn, &c, 1, &n, nullptr) || n == 0) {
      close_and_notify(*state);
      return;
    }

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
  st.read_h  = GetStdHandle(STD_INPUT_HANDLE);
  st.write_h = GetStdHandle(STD_OUTPUT_HANDLE);
  st.closed.store(false);
  st.stop_requested.store(false);
  st.hello_seen.store(false);

  if (const auto* f = params.findField("handshake_timeout_ms"_key);
      f != nullptr && f->is<int32_t>())
    st.handshake_timeout =
        std::chrono::milliseconds{std::max(0, f->as<int32_t>())};

  // Start reader before emitting HELLO so the server's response is not missed.
  if (!impl_->reader.joinable())
    impl_->reader = std::thread(client_reader_loop, impl_->state);

  emit_dcs(st.write_h, st,
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
  emit_data(impl_->state->write_h, *impl_->state, frame);
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
      emit_dcs(st.write_h, st,
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

  impl_->opened = false;
}

} // namespace bdg::bison::app

#endif // defined(_WIN32)
