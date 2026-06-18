// MIT License © 2025 Binary Dice Games
/**
 * @file pipe_transport.cpp
 * @brief Anonymous-pipe transport — platform-specific implementation.
 *
 * pipe_channel is defined here (not in the header) so that OS handle types
 * stay out of the public API.  shared_ptr's type-erased deleter means the
 * header can hold shared_ptr<pipe_channel> safely with an incomplete type.
 */
#include "src/rmi/transport/pipe_transport.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

// ── Platform-specific includes and pipe_channel definition ───────────────────

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace bdg::bison::rmi::transport {

/**
 * @brief Windows implementation: two anonymous pipes form a full-duplex channel.
 *
 * c2s: client writes → server reads.
 * s2c: server writes → client reads.
 */
struct pipe_channel {
  HANDLE c2s_r = INVALID_HANDLE_VALUE; ///< Server reads from here.
  HANDLE c2s_w = INVALID_HANDLE_VALUE; ///< Client writes here.
  HANDLE s2c_r = INVALID_HANDLE_VALUE; ///< Client reads from here.
  HANDLE s2c_w = INVALID_HANDLE_VALUE; ///< Server writes here.
  std::atomic<bool> closed{false};

  pipe_channel() {
    if (!CreatePipe(&c2s_r, &c2s_w, nullptr, 0) ||
        !CreatePipe(&s2c_r, &s2c_w, nullptr, 0)) {
      close_all();
      throw std::runtime_error("pipe_transport: CreatePipe failed");
    }
  }

  ~pipe_channel() { close_all(); }

  HANDLE client_read() const { return s2c_r; }
  HANDLE client_write() const { return c2s_w; }
  HANDLE server_read() const { return c2s_r; }
  HANDLE server_write() const { return s2c_w; }

 private:
  void close_all() {
    for (HANDLE h : {c2s_r, c2s_w, s2c_r, s2c_w})
      if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    c2s_r = c2s_w = s2c_r = s2c_w = INVALID_HANDLE_VALUE;
  }
};

// ── Windows I/O helpers ───────────────────────────────────────────────────────

namespace {

static void win_write_all(HANDLE h, const void* buf, DWORD size) {
  const auto* p = static_cast<const char*>(buf);
  DWORD done = 0;
  while (done < size) {
    DWORD n = 0;
    if (!WriteFile(h, p + done, size - done, &n, nullptr) || n == 0)
      throw std::runtime_error("pipe_transport: WriteFile failed");
    done += n;
  }
}

static bool win_read_all(
    HANDLE h,
    void* buf,
    DWORD size,
    const std::atomic<bool>& closed,
    std::chrono::steady_clock::time_point deadline) {
  auto* p = static_cast<char*>(buf);
  DWORD done = 0;
  while (done < size) {
    if (closed.load())
      return false;
    if (std::chrono::steady_clock::now() >= deadline)
      return false;

    DWORD available = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr))
      return false;
    if (available == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
      continue;
    }

    DWORD to_read = (std::min)(available, size - done);
    DWORD n = 0;
    if (!ReadFile(h, p + done, to_read, &n, nullptr))
      return false;
    done += n;
  }
  return true;
}

static constexpr DWORD kLenBytes = 4;

static void pipe_frame_write(HANDLE h, const bison::buffer& frame) {
  const uint32_t len_wire =
      bison::byte_swap(static_cast<uint32_t>(frame.size()));
  win_write_all(h, &len_wire, kLenBytes);
  if (!frame.empty())
    win_write_all(h, frame.data(), static_cast<DWORD>(frame.size()));
}

static bool pipe_frame_read(
    HANDLE h,
    bison::buffer& frame,
    const std::atomic<bool>& closed,
    std::chrono::steady_clock::time_point deadline) {
  uint32_t len_wire{};
  if (!win_read_all(h, &len_wire, kLenBytes, closed, deadline))
    return false;
  const uint32_t len = bison::byte_swap(len_wire);
  frame.resize(len);
  if (len > 0 &&
      !win_read_all(h, frame.data(), len, closed, deadline))
    return false;
  return true;
}

} // namespace

#else // ── POSIX (Linux) ──────────────────────────────────────────────────────

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace bdg::bison::rmi::transport {

/**
 * @brief POSIX implementation: two `pipe2` pairs form a full-duplex channel.
 *
 * c2s: c2s[1] (client writes) → c2s[0] (server reads).
 * s2c: s2c[1] (server writes) → s2c[0] (client reads).
 */
struct pipe_channel {
  int c2s[2] = {-1, -1}; ///< [0]=server reads, [1]=client writes.
  int s2c[2] = {-1, -1}; ///< [0]=client reads, [1]=server writes.
  std::atomic<bool> closed{false};

  pipe_channel() {
    if (::pipe2(c2s, O_CLOEXEC) != 0) {
      throw std::runtime_error(
          std::string("pipe_transport: pipe2 failed: ") + std::strerror(errno));
    }
    if (::pipe2(s2c, O_CLOEXEC) != 0) {
      ::close(c2s[0]);
      ::close(c2s[1]);
      throw std::runtime_error(
          std::string("pipe_transport: pipe2 failed: ") + std::strerror(errno));
    }
  }

  ~pipe_channel() {
    for (int fd : {c2s[0], c2s[1], s2c[0], s2c[1]})
      if (fd != -1)
        ::close(fd);
  }

  int client_read() const { return s2c[0]; }
  int client_write() const { return c2s[1]; }
  int server_read() const { return c2s[0]; }
  int server_write() const { return s2c[1]; }
};

// ── POSIX I/O helpers ─────────────────────────────────────────────────────────

namespace {

static void posix_write_all(int fd, const void* buf, std::size_t size) {
  const auto* p = static_cast<const char*>(buf);
  std::size_t done = 0;
  while (done < size) {
    ssize_t n = ::write(fd, p + done, size - done);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      throw std::runtime_error(
          std::string("pipe_transport: write failed: ") + std::strerror(errno));
    }
    done += static_cast<std::size_t>(n);
  }
}

static bool posix_read_all(
    int fd,
    void* buf,
    std::size_t size,
    const std::atomic<bool>& closed,
    std::chrono::steady_clock::time_point deadline) {
  auto* p = static_cast<char*>(buf);
  std::size_t done = 0;
  while (done < size) {
    if (closed.load())
      return false;
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      return false;

    // Poll for at most 10 ms per slice so the closed flag is checked regularly.
    auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count();
    int slice_ms = static_cast<int>((std::min)(remaining_ms, (long long)10));

    struct pollfd pfd{fd, POLLIN, 0};
    int r = ::poll(&pfd, 1, slice_ms);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (r == 0)
      continue; // timeout slice — loop to recheck closed/deadline

    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
      return false;
    if (!(pfd.revents & POLLIN))
      continue;

    ssize_t n = ::read(fd, p + done, size - done);
    if (n == 0)
      return false; // EOF — write end was closed
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  return true;
}

static constexpr std::size_t kLenBytes = 4;

static void pipe_frame_write(int fd, const bison::buffer& frame) {
  const uint32_t len_wire =
      bison::byte_swap(static_cast<uint32_t>(frame.size()));
  posix_write_all(fd, &len_wire, kLenBytes);
  if (!frame.empty())
    posix_write_all(fd, frame.data(), frame.size());
}

static bool pipe_frame_read(
    int fd,
    bison::buffer& frame,
    const std::atomic<bool>& closed,
    std::chrono::steady_clock::time_point deadline) {
  uint32_t len_wire{};
  if (!posix_read_all(fd, &len_wire, kLenBytes, closed, deadline))
    return false;
  const uint32_t len = bison::byte_swap(len_wire);
  frame.resize(len);
  if (len > 0 &&
      !posix_read_all(fd, frame.data(), len, closed, deadline))
    return false;
  return true;
}

} // namespace

#endif // platform

// ── pipe_client_transport ─────────────────────────────────────────────────────

/** @copydoc bdg::bison::rmi::transport::pipe_client_transport::pipe_client_transport */
pipe_client_transport::pipe_client_transport(std::shared_ptr<pipe_channel> ch)
    : ch_(std::move(ch)) {}

/** @copydoc bdg::bison::rmi::transport::pipe_client_transport::open */
void pipe_client_transport::open(bison::dynamic /*params*/) {}

/** @copydoc bdg::bison::rmi::transport::pipe_client_transport::send */
void pipe_client_transport::send(bison::buffer frame) {
  std::lock_guard<std::mutex> lk(send_mtx_);
  pipe_frame_write(ch_->client_write(), frame);
}

/** @copydoc bdg::bison::rmi::transport::pipe_client_transport::receive */
bool pipe_client_transport::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  std::lock_guard<std::mutex> lk(recv_mtx_);
  return pipe_frame_read(
      ch_->client_read(), frame, ch_->closed,
      std::chrono::steady_clock::now() + timeout);
}

/** @copydoc bdg::bison::rmi::transport::pipe_client_transport::shutdown */
void pipe_client_transport::shutdown() {
  ch_->closed.store(true);
}

// ── pipe_server_connection ────────────────────────────────────────────────────

/** @copydoc bdg::bison::rmi::transport::pipe_server_connection::pipe_server_connection */
pipe_server_connection::pipe_server_connection(std::shared_ptr<pipe_channel> ch)
    : ch_(std::move(ch)) {}

/** @copydoc bdg::bison::rmi::transport::pipe_server_connection::send */
void pipe_server_connection::send(bison::buffer frame) {
  std::lock_guard<std::mutex> lk(send_mtx_);
  pipe_frame_write(ch_->server_write(), frame);
}

/** @copydoc bdg::bison::rmi::transport::pipe_server_connection::receive */
bool pipe_server_connection::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  std::lock_guard<std::mutex> lk(recv_mtx_);
  return pipe_frame_read(
      ch_->server_read(), frame, ch_->closed,
      std::chrono::steady_clock::now() + timeout);
}

/** @copydoc bdg::bison::rmi::transport::pipe_server_connection::close */
void pipe_server_connection::close() {
  ch_->closed.store(true);
}

/** @copydoc bdg::bison::rmi::transport::pipe_server_connection::is_closed */
bool pipe_server_connection::is_closed() const {
  return ch_->closed.load();
}

// ── pipe_server_transport ─────────────────────────────────────────────────────

/** @copydoc bdg::bison::rmi::transport::pipe_server_transport::start */
void pipe_server_transport::start(bison::dynamic /*params*/) {
  stopped_.store(false);
}

/** @copydoc bdg::bison::rmi::transport::pipe_server_transport::connect */
pipe_client_transport pipe_server_transport::connect() {
  auto ch = std::make_shared<pipe_channel>();
  {
    std::lock_guard<std::mutex> lk(mtx_);
    pending_.push(ch);
  }
  cv_.notify_one();
  return pipe_client_transport{std::move(ch)};
}

/** @copydoc bdg::bison::rmi::transport::pipe_server_transport::accept */
std::unique_ptr<server_connection_iface> pipe_server_transport::accept(
    std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lk(mtx_);
  if (!cv_.wait_for(lk, timeout,
                    [this] { return !pending_.empty() || stopped_.load(); }))
    return nullptr;
  if (pending_.empty())
    return nullptr;
  auto ch = std::move(pending_.front());
  pending_.pop();
  return std::make_unique<pipe_server_connection>(std::move(ch));
}

/** @copydoc bdg::bison::rmi::transport::pipe_server_transport::stop */
void pipe_server_transport::stop() {
  stopped_.store(true);
  cv_.notify_all();
}

} // namespace bdg::bison::rmi::transport
