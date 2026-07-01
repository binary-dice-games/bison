// MIT License © 2025 Binary Dice Games
/**
 * @file pty_server_transport_linux.cpp
 * @brief Linux implementation of pty_server_transport using forkpty/read/write.
 *
 * Two background threads are launched per connection:
 *  - read_thread:         PTY master → dcs_byte_parser → inbox / on_plain
 *  - stdin_relay_thread:  server stdin → PTY master (forwards keystrokes to child)
 *
 * The server's own stdin is put in raw mode while the connection is live so
 * that individual keypresses are forwarded immediately without line buffering.
 * The original termios settings are restored when the connection closes.
 */
#ifdef __linux__

#include "src/rmi/transport/pty_server_transport.hpp"
#include "src/rmi/transport/pty_channel_state.hpp"
#include "src/rmi/transport/dcs_framing.hpp"

#include <optional>
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
  std::function<void()> on_closed;
  std::thread stdin_relay_thread;
  std::optional<struct termios> saved_stdin_termios;

  explicit pty_server_conn_impl(pty::pty_process p,
                                std::function<void(uint8_t)> plain_cb,
                                std::function<void()> closed_cb)
      : process(std::move(p)),
        master_fd(process.release_master_fd()),
        on_plain(std::move(plain_cb)),
        on_closed(std::move(closed_cb)) {}
};

// ── Background threads ────────────────────────────────────────────────────────

static void run_read_thread(int master_fd,
                            pty_channel_state& st,
                            std::function<void(uint8_t)> on_plain,
                            std::function<void()> on_closed) {
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
  if (on_closed)
    on_closed();
}

// Relay server stdin → PTY master so keystrokes reach the child process.
static void run_stdin_relay_thread(int master_fd, pty_channel_state& st) {
  uint8_t buf[256];
  while (!st.stopped.load()) {
    pollfd pfd{};
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 100) <= 0)
      continue;
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0)
      break;
    for (ssize_t off = 0; off < n; ) {
      const ssize_t w = ::write(master_fd, buf + off, static_cast<size_t>(n - off));
      if (w <= 0)
        return;
      off += w;
    }
  }
}

// ── pty_server_connection ─────────────────────────────────────────────────────

pty_server_connection::pty_server_connection(
    std::unique_ptr<pty_server_conn_impl> impl)
    : impl_(std::move(impl)) {
  // Set the PTY master to raw mode: no echo, no newline translation.
  struct termios t{};
  if (tcgetattr(impl_->master_fd, &t) == 0) {
    cfmakeraw(&t);
    tcsetattr(impl_->master_fd, TCSANOW, &t);
  }

  // Put the server's own stdin in raw mode so individual keypresses are
  // forwarded to the child immediately, without waiting for Enter.
  if (isatty(STDIN_FILENO)) {
    struct termios s{};
    if (tcgetattr(STDIN_FILENO, &s) == 0) {
      impl_->saved_stdin_termios = s;
      cfmakeraw(&s);
      tcsetattr(STDIN_FILENO, TCSANOW, &s);
    }
  }

  impl_->st.read_thread = std::thread(
      run_read_thread, impl_->master_fd, std::ref(impl_->st),
      impl_->on_plain, impl_->on_closed);
  impl_->stdin_relay_thread = std::thread(
      run_stdin_relay_thread, impl_->master_fd, std::ref(impl_->st));
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
  // Closing master_fd unblocks the read thread's poll().
  // The stdin relay exits within one 100 ms poll timeout.
  if (impl_->master_fd >= 0) {
    ::close(impl_->master_fd);
    impl_->master_fd = -1;
  }
  if (impl_->st.read_thread.joinable())
    impl_->st.read_thread.join();
  if (impl_->stdin_relay_thread.joinable())
    impl_->stdin_relay_thread.join();
  // Restore the server's stdin to its original terminal settings.
  if (impl_->saved_stdin_termios) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &*impl_->saved_stdin_termios);
    impl_->saved_stdin_termios.reset();
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

#endif // __linux__
