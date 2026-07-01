// MIT License © 2025 Binary Dice Games
/**
 * @file pty_server_transport_win.cpp
 * @brief Windows implementation of pty_server_transport using ConPTY.
 *
 * Reads child output from the ConPTY output pipe (h_out_read) on a background
 * thread using PeekNamedPipe + ReadFile polling.  Writes DCS frames to the
 * ConPTY input pipe (h_in_write) directly via WriteFile under a mutex.
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
  std::function<void(uint8_t)> on_plain;

  explicit pty_server_conn_impl(pty::pty_process p,
                                std::function<void(uint8_t)> cb)
      : process(std::move(p)), on_plain(std::move(cb)) {
    process.release_handles(h_out_read, h_in_write); // transfer ownership
  }

  ~pty_server_conn_impl() {
    // Join the read thread before closing the handles it uses.
    st.stopped.store(true);
    if (st.read_thread.joinable())
      st.read_thread.join();
    if (h_out_read != INVALID_HANDLE_VALUE) {
      CloseHandle(h_out_read);
      h_out_read = INVALID_HANDLE_VALUE;
    }
    if (h_in_write != INVALID_HANDLE_VALUE) {
      CloseHandle(h_in_write);
      h_in_write = INVALID_HANDLE_VALUE;
    }
    // st destructor runs next; thread is already joined so it's a no-op.
  }
};

// ── Background read thread ────────────────────────────────────────────────────

static void run_read_thread(HANDLE h_out,
                            pty_channel_state& st,
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
  dcs::close_and_notify(st);
}

// ── pty_server_connection ─────────────────────────────────────────────────────

pty_server_connection::pty_server_connection(
    std::unique_ptr<pty_server_conn_impl> impl)
    : impl_(std::move(impl)) {
  impl_->st.read_thread = std::thread(
      run_read_thread,
      impl_->h_out_read,
      std::ref(impl_->st),
      impl_->on_plain);
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
  if (impl_->st.read_thread.joinable())
    impl_->st.read_thread.join();
  dcs::close_and_notify(impl_->st);
}

bool pty_server_connection::is_closed() const {
  return impl_->st.closed.load();
}

// ── pty_server_transport ──────────────────────────────────────────────────────

pty_server_transport::pty_server_transport(pty::pty_config cfg, plain_cb on_plain)
    : cfg_(std::move(cfg)), on_plain_(std::move(on_plain)) {}

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
                                                     on_plain_);
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
