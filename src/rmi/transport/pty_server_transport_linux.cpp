// MIT License © 2025 Binary Dice Games
/**
 * @file pty_server_transport_linux.cpp
 * @brief Linux implementation of pty_server_transport using forkpty/read/write.
 *
 * The read thread polls the PTY master fd (with a 100 ms timeout so it can
 * detect shutdown), feeds each byte to dcs_byte_parser, and pushes assembled
 * frames into the inbox queue.  Writes go directly to the master fd under a
 * mutex; no libuv involvement on the write path.
 */
#ifdef __linux__

#include "src/rmi/transport/pty_server_transport.hpp"
#include "src/rmi/transport/pty_channel_state.hpp"
#include "src/rmi/transport/dcs_framing.hpp"

#include <stdexcept>

#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace bdg {
namespace bison {
namespace rmi {
namespace transport {

// ── Per-connection impl ───────────────────────────────────────────────────────

struct pty_server_conn_impl {
  pty::pty_process process;
  pty_channel_state st;
  int master_fd;
  std::function<void(uint8_t)> on_plain;

  explicit pty_server_conn_impl(pty::pty_process p,
                                std::function<void(uint8_t)> cb)
      : process(std::move(p)),
        master_fd(process.release_master_fd()), // transfer ownership; process dtor won't close
        on_plain(std::move(cb)) {}
};

// ── Background read thread ────────────────────────────────────────────────────

static void run_read_thread(int master_fd,
                            pty_channel_state& st,
                            std::function<void(uint8_t)> on_plain) {
  dcs::dcs_byte_parser parser(
      [&st](const std::string& body) { dcs::process_body(st, body); },
      std::move(on_plain));

  uint8_t buf[4096];
  while (!st.stopped.load()) {
    pollfd pfd{};
    pfd.fd = master_fd;
    pfd.events = POLLIN;
    const int ready = poll(&pfd, 1, 100);
    if (ready < 0)
      break;
    if (ready == 0)
      continue;
    const ssize_t n = ::read(master_fd, buf, sizeof(buf));
    if (n <= 0)
      break;
    for (ssize_t i = 0; i < n; ++i)
      parser.feed(buf[static_cast<size_t>(i)]);
  }
  dcs::close_and_notify(st);
}

// ── pty_server_connection ─────────────────────────────────────────────────────

pty_server_connection::pty_server_connection(
    std::unique_ptr<pty_server_conn_impl> impl)
    : impl_(std::move(impl)) {
  // Set the PTY to raw mode so DCS bytes are not altered by the line discipline
  // (no echo, no newline translation, no signal generation).
  struct termios t{};
  if (tcgetattr(impl_->master_fd, &t) == 0) {
    cfmakeraw(&t);
    tcsetattr(impl_->master_fd, TCSANOW, &t);
  }
  // Launch the background read thread.
  impl_->st.read_thread = std::thread(
      run_read_thread,
      impl_->master_fd,
      std::ref(impl_->st),
      impl_->on_plain);
}

pty_server_connection::~pty_server_connection() {
  close();
}

void pty_server_connection::send(bison::buffer frame) {
  if (impl_->st.closed.load())
    throw std::runtime_error("pty_server_connection::send: connection closed");
  dcs::emit_data(impl_->master_fd, impl_->st, frame);
}

bool pty_server_connection::receive(bison::buffer& frame,
                                    std::chrono::milliseconds timeout) {
  return impl_->st.dequeue(frame, timeout);
}

void pty_server_connection::close() {
  impl_->st.stopped.store(true);
  // Closing the master fd unblocks any pending read() in the read thread.
  if (impl_->master_fd >= 0) {
    ::close(impl_->master_fd);
    impl_->master_fd = -1;
  }
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

#endif // __linux__
