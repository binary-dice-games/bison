// MIT License © 2025 Binary Dice Games
/**
 * @file pty_client_transport_linux.cpp
 * @brief Linux implementation of pty_client_transport.
 *
 * Reads DCS frames from STDIN_FILENO (the PTY slave) using a background
 * thread with poll()/read().  Writes DCS frames to STDOUT_FILENO directly
 * via write_all_fd.
 */
#ifdef __linux__

#include "src/rmi/transport/pty_client_transport.hpp"
#include "src/rmi/transport/pty_channel_state.hpp"
#include "src/rmi/transport/dcs_framing.hpp"

#include <stdexcept>

#include <poll.h>
#include <unistd.h>

namespace bdg {
namespace bison {
namespace rmi {
namespace transport {

// ── Per-transport impl ────────────────────────────────────────────────────────

struct pty_client_impl {
  pty_channel_state st;
  std::function<void(uint8_t)> on_plain;
  bool opened{false};
};

// ── Background read thread ────────────────────────────────────────────────────

static void run_client_read(pty_channel_state& st,
                            std::function<void(uint8_t)> on_plain) {
  dcs::dcs_byte_parser parser(
      [&st](const std::string& body) { dcs::process_body(st, body); },
      std::move(on_plain));

  uint8_t buf[4096];
  while (!st.stopped.load()) {
    pollfd pfd{};
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    const int ready = poll(&pfd, 1, 100);
    if (ready < 0)
      break;
    if (ready == 0)
      continue;
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0)
      break;
    for (ssize_t i = 0; i < n; ++i)
      parser.feed(buf[static_cast<size_t>(i)]);
  }
  dcs::close_and_notify(st);
}

// ── pty_client_transport ──────────────────────────────────────────────────────

pty_client_transport::pty_client_transport(plain_cb on_plain)
    : impl_(std::make_unique<pty_client_impl>()) {
  impl_->on_plain = std::move(on_plain);
}

pty_client_transport::~pty_client_transport() {
  shutdown();
}

void pty_client_transport::open(bison::dynamic /*params*/) {
  if (impl_->opened)
    return;
  impl_->opened = true;
  impl_->st.read_thread = std::thread(
      run_client_read,
      std::ref(impl_->st),
      impl_->on_plain);
  // Send HELLO to the server so it knows the client is ready.
  dcs::emit_dcs(STDOUT_FILENO, impl_->st,
                std::string{dcs::kProtoVersion} + ";type=HELLO");
}

void pty_client_transport::send(bison::buffer frame) {
  if (!impl_->opened)
    throw std::runtime_error("pty_client_transport::send: not opened");
  if (impl_->st.closed.load())
    throw std::runtime_error("pty_client_transport::send: connection closed");
  dcs::emit_data(STDOUT_FILENO, impl_->st, frame);
}

bool pty_client_transport::receive(bison::buffer& frame,
                                   std::chrono::milliseconds timeout) {
  return impl_->st.dequeue(frame, timeout);
}

void pty_client_transport::shutdown() {
  impl_->st.stopped.store(true);
  if (impl_->st.read_thread.joinable())
    impl_->st.read_thread.join();
  dcs::close_and_notify(impl_->st);
}

bool pty_client_transport::is_connected() const {
  return !impl_->st.closed.load();
}

} // namespace transport
} // namespace rmi
} // namespace bison
} // namespace bdg

#endif // __linux__
