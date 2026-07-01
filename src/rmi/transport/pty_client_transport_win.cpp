// MIT License © 2025 Binary Dice Games
/**
 * @file pty_client_transport_win.cpp
 * @brief Windows implementation of pty_client_transport.
 *
 * Reads DCS frames from the process's standard input handle (the ConPTY
 * slave) on a background thread using PeekNamedPipe + ReadFile polling.
 * Writes DCS frames to the standard output handle via WriteFile.
 */
#ifdef _WIN32

#include "src/rmi/transport/pty_client_transport.hpp"
#include "src/rmi/transport/pty_channel_state.hpp"
#include "src/rmi/transport/dcs_framing.hpp"

#include <stdexcept>

#include <windows.h>
#include <io.h>

namespace bdg {
namespace bison {
namespace rmi {
namespace transport {

// ── Per-transport impl ────────────────────────────────────────────────────────

struct pty_client_impl {
  pty_channel_state st;
  std::function<void(uint8_t)> on_plain;
  HANDLE h_stdin{INVALID_HANDLE_VALUE};
  HANDLE h_stdout{INVALID_HANDLE_VALUE};
  bool opened{false};
};

// ── Background read thread ────────────────────────────────────────────────────

static void run_client_read(HANDLE h_stdin,
                            pty_channel_state& st,
                            std::function<void(uint8_t)> on_plain) {
  dcs::dcs_byte_parser parser(
      [&st](const std::string& body) { dcs::process_body(st, body); },
      std::move(on_plain));

  BYTE buf[4096];
  while (!st.stopped.load()) {
    DWORD avail = 0;
    if (!PeekNamedPipe(h_stdin, nullptr, 0, nullptr, &avail, nullptr)) {
      // Peek failed — may be a console handle rather than a pipe.
      // Fall back to a blocking ReadFile with a short timeout.
      DWORD n = 0;
      if (!ReadFile(h_stdin, buf, 1, &n, nullptr) || n == 0)
        break;
      for (DWORD i = 0; i < n; ++i)
        parser.feed(static_cast<uint8_t>(buf[i]));
      continue;
    }
    if (avail == 0) {
      Sleep(10);
      continue;
    }
    DWORD n = 0;
    const DWORD to_read = avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf));
    if (!ReadFile(h_stdin, buf, to_read, &n, nullptr) || n == 0)
      break;
    for (DWORD i = 0; i < n; ++i)
      parser.feed(static_cast<uint8_t>(buf[i]));
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
  impl_->h_stdin = GetStdHandle(STD_INPUT_HANDLE);
  impl_->h_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
  impl_->st.read_thread = std::thread(
      run_client_read,
      impl_->h_stdin,
      std::ref(impl_->st),
      impl_->on_plain);
  // Send HELLO to the server.
  dcs::emit_dcs(impl_->h_stdout, impl_->st,
                std::string{dcs::kProtoVersion} + ";type=HELLO");
}

void pty_client_transport::send(bison::buffer frame) {
  if (!impl_->opened)
    throw std::runtime_error("pty_client_transport::send: not opened");
  if (impl_->st.closed.load())
    throw std::runtime_error("pty_client_transport::send: connection closed");
  dcs::emit_data(impl_->h_stdout, impl_->st, frame);
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

#endif // _WIN32
