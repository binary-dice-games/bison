// MIT License © 2025 Binary Dice Games
/**
 * @file named_pipe_transport.cpp
 * @brief Named-pipe / Unix-socket transport — platform-specific implementation.
 *
 * Windows: Win32 named pipes (CreateNamedPipe / ConnectNamedPipe).
 * Linux/macOS: Unix domain sockets (AF_UNIX, SOCK_STREAM).
 *
 * Framing: 4-byte big-endian length prefix followed by the payload, matching
 * pipe_transport and stream_transport.
 */
#include "src/rmi/transport/named_pipe_transport.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

// ── Platform-specific includes and types ─────────────────────────────────────

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace bdg::bison::rmi::transport {

struct named_pipe_conn {
  HANDLE hPipe = INVALID_HANDLE_VALUE;
  std::atomic<bool> closed{false};

  ~named_pipe_conn() {
    if (hPipe != INVALID_HANDLE_VALUE) {
      DisconnectNamedPipe(hPipe);
      CloseHandle(hPipe);
    }
  }
};

struct named_pipe_server_state {
  HANDLE stop_event = INVALID_HANDLE_VALUE;

  named_pipe_server_state() {
    stop_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == INVALID_HANDLE_VALUE)
      throw std::runtime_error("named_pipe: CreateEvent failed");
  }
  ~named_pipe_server_state() {
    if (stop_event != INVALID_HANDLE_VALUE)
      CloseHandle(stop_event);
  }
};

// ── Windows framing helpers ───────────────────────────────────────────────────

namespace {

static void win_write_all(HANDLE h, const void* buf, DWORD size) {
  const auto* p = static_cast<const char*>(buf);
  DWORD done = 0;
  while (done < size) {
    DWORD n = 0;
    if (!WriteFile(h, p + done, size - done, &n, nullptr) || n == 0)
      throw std::runtime_error("named_pipe: WriteFile failed");
    done += n;
  }
}

static bool win_read_all(
    HANDLE h, void* buf, DWORD size,
    const std::atomic<bool>& closed,
    std::chrono::steady_clock::time_point deadline) {
  auto* p = static_cast<char*>(buf);
  DWORD done = 0;
  while (done < size) {
    if (closed.load()) return false;
    if (std::chrono::steady_clock::now() >= deadline) return false;

    DWORD available = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr))
      return false; // pipe broken
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

static void win_frame_write(HANDLE h, const bison::buffer& frame) {
  const uint32_t len_wire =
      bison::byte_swap(static_cast<uint32_t>(frame.size()));
  win_write_all(h, &len_wire, 4);
  if (!frame.empty())
    win_write_all(h, frame.data(), static_cast<DWORD>(frame.size()));
}

static bool win_frame_read(
    HANDLE h, bison::buffer& frame,
    const std::atomic<bool>& closed,
    std::chrono::steady_clock::time_point deadline) {
  uint32_t len_wire{};
  if (!win_read_all(h, &len_wire, 4, closed, deadline)) return false;
  const uint32_t len = bison::byte_swap(len_wire);
  frame.resize(len);
  return len == 0 || win_read_all(h, frame.data(), len, closed, deadline);
}

} // namespace

#else // ── POSIX (Linux / macOS) ──────────────────────────────────────────────

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace bdg::bison::rmi::transport {

struct named_pipe_conn {
  int fd = -1;
  std::atomic<bool> closed{false};

  ~named_pipe_conn() {
    if (fd != -1) ::close(fd);
  }
};

struct named_pipe_server_state {
  int server_fd = -1;
  int stop_pipe[2] = {-1, -1};

  named_pipe_server_state() {
    if (::pipe(stop_pipe) != 0)
      throw std::runtime_error("named_pipe: pipe() for stop_pipe failed");
  }

  ~named_pipe_server_state() {
    if (server_fd != -1) ::close(server_fd);
    if (stop_pipe[0] != -1) ::close(stop_pipe[0]);
    if (stop_pipe[1] != -1) ::close(stop_pipe[1]);
  }
};

// ── POSIX framing helpers ─────────────────────────────────────────────────────

namespace {

static void posix_write_all(int fd, const void* buf, std::size_t size) {
  const auto* p = static_cast<const char*>(buf);
  std::size_t done = 0;
  while (done < size) {
    ssize_t n = ::write(fd, p + done, size - done);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      throw std::runtime_error(
          std::string("named_pipe: write failed: ") + std::strerror(errno));
    }
    done += static_cast<std::size_t>(n);
  }
}

static bool posix_read_all(
    int fd, void* buf, std::size_t size,
    const std::atomic<bool>& closed,
    std::chrono::steady_clock::time_point deadline) {
  auto* p = static_cast<char*>(buf);
  std::size_t done = 0;
  while (done < size) {
    if (closed.load()) return false;
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return false;

    auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    int slice_ms = static_cast<int>((std::min)(remaining_ms, decltype(remaining_ms){10}));

    struct pollfd pfd{fd, POLLIN, 0};
    int r = ::poll(&pfd, 1, slice_ms);
    if (r < 0) { if (errno == EINTR) continue; return false; }
    if (r == 0) continue;
    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) return false;
    if (!(pfd.revents & POLLIN)) continue;

    ssize_t n = ::read(fd, p + done, size - done);
    if (n == 0) return false; // EOF
    if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; return false; }
    done += static_cast<std::size_t>(n);
  }
  return true;
}

static void posix_frame_write(int fd, const bison::buffer& frame) {
  const uint32_t len_wire =
      bison::byte_swap(static_cast<uint32_t>(frame.size()));
  posix_write_all(fd, &len_wire, 4);
  if (!frame.empty())
    posix_write_all(fd, frame.data(), frame.size());
}

static bool posix_frame_read(
    int fd, bison::buffer& frame,
    const std::atomic<bool>& closed,
    std::chrono::steady_clock::time_point deadline) {
  uint32_t len_wire{};
  if (!posix_read_all(fd, &len_wire, 4, closed, deadline)) return false;
  const uint32_t len = bison::byte_swap(len_wire);
  frame.resize(len);
  return len == 0 || posix_read_all(fd, frame.data(), len, closed, deadline);
}

} // namespace

#endif // platform

// ── named_pipe_server_connection ──────────────────────────────────────────────

named_pipe_server_connection::named_pipe_server_connection(
    std::unique_ptr<named_pipe_conn> conn)
    : conn_(std::move(conn)) {}

named_pipe_server_connection::~named_pipe_server_connection() = default;

void named_pipe_server_connection::send(bison::buffer frame) {
  std::lock_guard<std::mutex> lk(send_mtx_);
#if defined(_WIN32)
  win_frame_write(conn_->hPipe, frame);
#else
  posix_frame_write(conn_->fd, frame);
#endif
}

bool named_pipe_server_connection::receive(
    bison::buffer& frame, std::chrono::milliseconds timeout) {
  std::lock_guard<std::mutex> lk(recv_mtx_);
  auto deadline = std::chrono::steady_clock::now() + timeout;
#if defined(_WIN32)
  return win_frame_read(conn_->hPipe, frame, conn_->closed, deadline);
#else
  return posix_frame_read(conn_->fd, frame, conn_->closed, deadline);
#endif
}

void named_pipe_server_connection::close() {
  conn_->closed.store(true);
#if defined(_WIN32)
  if (conn_->hPipe != INVALID_HANDLE_VALUE) {
    DisconnectNamedPipe(conn_->hPipe);
    CloseHandle(conn_->hPipe);
    conn_->hPipe = INVALID_HANDLE_VALUE;
  }
#else
  if (conn_->fd != -1) {
    ::shutdown(conn_->fd, SHUT_RDWR);
    ::close(conn_->fd);
    conn_->fd = -1;
  }
#endif
}

bool named_pipe_server_connection::is_closed() const {
  return conn_->closed.load();
}

// ── named_pipe_client_transport ───────────────────────────────────────────────

named_pipe_client_transport::named_pipe_client_transport(std::string path)
    : path_(std::move(path)), conn_(std::make_unique<named_pipe_conn>()) {}

named_pipe_client_transport::~named_pipe_client_transport() = default;

void named_pipe_client_transport::open(bison::dynamic /*params*/) {
#if defined(_WIN32)
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (std::chrono::steady_clock::now() < deadline) {
    conn_->hPipe = CreateFileA(
        path_.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (conn_->hPipe != INVALID_HANDLE_VALUE) return;

    DWORD err = GetLastError();
    if (err == ERROR_PIPE_BUSY) {
      WaitNamedPipeA(path_.c_str(), 200);
      continue;
    }
    break;
  }
  throw std::runtime_error("named_pipe: failed to connect to " + path_);

#else
  conn_->fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (conn_->fd < 0)
    throw std::runtime_error(
        std::string("named_pipe: socket() failed: ") + std::strerror(errno));

  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path_.size() >= sizeof(addr.sun_path))
    throw std::runtime_error("named_pipe: path too long");
  path_.copy(addr.sun_path, path_.size());

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (std::chrono::steady_clock::now() < deadline) {
    if (::connect(conn_->fd,
                  reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) == 0)
      return;
    if (errno != ENOENT && errno != ECONNREFUSED) break;
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  }
  ::close(conn_->fd);
  conn_->fd = -1;
  throw std::runtime_error("named_pipe: failed to connect to " + path_);
#endif
}

void named_pipe_client_transport::send(bison::buffer frame) {
  std::lock_guard<std::mutex> lk(send_mtx_);
#if defined(_WIN32)
  win_frame_write(conn_->hPipe, frame);
#else
  posix_frame_write(conn_->fd, frame);
#endif
}

bool named_pipe_client_transport::receive(
    bison::buffer& frame, std::chrono::milliseconds timeout) {
  std::lock_guard<std::mutex> lk(recv_mtx_);
  auto deadline = std::chrono::steady_clock::now() + timeout;
#if defined(_WIN32)
  return win_frame_read(conn_->hPipe, frame, conn_->closed, deadline);
#else
  return posix_frame_read(conn_->fd, frame, conn_->closed, deadline);
#endif
}

void named_pipe_client_transport::shutdown() {
  conn_->closed.store(true);
#if defined(_WIN32)
  if (conn_->hPipe != INVALID_HANDLE_VALUE) {
    CloseHandle(conn_->hPipe);
    conn_->hPipe = INVALID_HANDLE_VALUE;
  }
#else
  if (conn_->fd != -1) {
    ::shutdown(conn_->fd, SHUT_RDWR);
    ::close(conn_->fd);
    conn_->fd = -1;
  }
#endif
}

// ── named_pipe_server_transport ───────────────────────────────────────────────

named_pipe_server_transport::named_pipe_server_transport(std::string path)
    : path_(std::move(path)) {}

named_pipe_server_transport::~named_pipe_server_transport() {
  stop();
}

void named_pipe_server_transport::start(bison::dynamic /*params*/) {
  stopped_.store(false);
  state_ = std::make_unique<named_pipe_server_state>();

#if !defined(_WIN32)
  // Bind the Unix socket so clients can connect.
  ::unlink(path_.c_str()); // remove stale socket file if present

  state_->server_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (state_->server_fd < 0)
    throw std::runtime_error(
        std::string("named_pipe: socket() failed: ") + std::strerror(errno));

  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path_.size() >= sizeof(addr.sun_path))
    throw std::runtime_error("named_pipe: path too long");
  path_.copy(addr.sun_path, path_.size());

  if (::bind(state_->server_fd,
             reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) != 0) {
    ::close(state_->server_fd);
    state_->server_fd = -1;
    throw std::runtime_error(
        std::string("named_pipe: bind() failed: ") + std::strerror(errno));
  }
  if (::listen(state_->server_fd, SOMAXCONN) != 0) {
    ::close(state_->server_fd);
    state_->server_fd = -1;
    throw std::runtime_error(
        std::string("named_pipe: listen() failed: ") + std::strerror(errno));
  }
#endif
}

std::unique_ptr<server_connection_iface> named_pipe_server_transport::accept(
    std::chrono::milliseconds timeout) {
  if (stopped_.load()) return nullptr;

#if defined(_WIN32)
  // Each accept() creates a fresh pipe instance so multiple clients can connect.
  HANDLE hPipe = CreateNamedPipeA(
      path_.c_str(),
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      PIPE_UNLIMITED_INSTANCES,
      65536, 65536, 0, nullptr);
  if (hPipe == INVALID_HANDLE_VALUE)
    throw std::runtime_error("named_pipe: CreateNamedPipe failed for " + path_);

  OVERLAPPED ov{};
  ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (ov.hEvent == INVALID_HANDLE_VALUE) {
    CloseHandle(hPipe);
    throw std::runtime_error("named_pipe: CreateEvent failed");
  }

  BOOL ok = ConnectNamedPipe(hPipe, &ov);
  if (!ok) {
    DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) {
      SetEvent(ov.hEvent); // client connected before our call
    } else if (err != ERROR_IO_PENDING) {
      CloseHandle(ov.hEvent);
      CloseHandle(hPipe);
      return nullptr;
    }
  }

  HANDLE waitHandles[2] = {ov.hEvent, state_->stop_event};
  DWORD result = WaitForMultipleObjects(
      2, waitHandles, FALSE, static_cast<DWORD>(timeout.count()));

  if (result == WAIT_OBJECT_0) {
    // Client connected.
    CloseHandle(ov.hEvent);
    auto conn = std::make_unique<named_pipe_conn>();
    conn->hPipe = hPipe;
    return std::make_unique<named_pipe_server_connection>(std::move(conn));
  }

  // Timeout or stop: cancel pending IO and clean up.
  CancelIo(hPipe);
  WaitForSingleObject(ov.hEvent, INFINITE);
  CloseHandle(ov.hEvent);
  CloseHandle(hPipe);
  return nullptr;

#else
  // Wait for a client using poll() so we can honour the timeout and the stop pipe.
  struct pollfd fds[2];
  fds[0] = {state_->server_fd, POLLIN, 0};
  fds[1] = {state_->stop_pipe[0], POLLIN, 0};

  int r = ::poll(fds, 2, static_cast<int>(timeout.count()));
  if (r <= 0 || stopped_.load()) return nullptr;
  if (!(fds[0].revents & POLLIN)) return nullptr;

  int client_fd = ::accept(state_->server_fd, nullptr, nullptr);
  if (client_fd < 0) return nullptr;

  auto conn = std::make_unique<named_pipe_conn>();
  conn->fd = client_fd;
  return std::make_unique<named_pipe_server_connection>(std::move(conn));
#endif
}

void named_pipe_server_transport::stop() {
  if (stopped_.exchange(true)) return;

#if defined(_WIN32)
  if (state_) SetEvent(state_->stop_event);
#else
  if (state_) {
    if (state_->stop_pipe[1] != -1) {
      char b = 0;
      (void)::write(state_->stop_pipe[1], &b, 1);
    }
    if (state_->server_fd != -1) {
      ::shutdown(state_->server_fd, SHUT_RDWR);
      ::close(state_->server_fd);
      state_->server_fd = -1;
      ::unlink(path_.c_str());
    }
  }
#endif
}

} // namespace bdg::bison::rmi::transport
