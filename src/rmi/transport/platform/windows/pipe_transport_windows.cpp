// MIT License © 2025 Binary Dice Games
/**
 * @file pipe_transport_windows.cpp
 * @brief Windows named pipe transport implementation.
 */
#include "src/rmi/transport/pipe_transport.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace bdg::bison::rmi::transport {

#ifdef _WIN32
namespace {

constexpr DWORD kBufferSize = 64 * 1024;

std::string read_pipe_name(
    const bison::dynamic& params,
    std::string fallback) {
  if (const auto* f = params.findField("pipe"_key); f != nullptr && f->is<std::string>()) {
    return f->as<std::string>();
  }
  if (const auto* f = params.findField("name"_key); f != nullptr && f->is<std::string>()) {
    return f->as<std::string>();
  }
  if (const auto* f = params.findField("__pipe"_key); f != nullptr && f->is<std::string>()) {
    return f->as<std::string>();
  }
  return fallback;
}

bool write_all(HANDLE pipe, const uint8_t* data, size_t size) {
  size_t sent = 0;
  while (sent < size) {
    DWORD written = 0;
    if (!::WriteFile(
            pipe,
            data + sent,
            static_cast<DWORD>(size - sent),
            &written,
            nullptr)) {
      return false;
    }
    if (written == 0) {
      return false;
    }
    sent += written;
  }
  return true;
}

bool read_exact(
    HANDLE pipe,
    uint8_t* out,
    size_t size,
    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  size_t got = 0;

  while (got < size) {
    DWORD available = 0;
    if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
      return false;
    }

    if (available == 0) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
      continue;
    }

    DWORD read = 0;
    const DWORD to_read = static_cast<DWORD>(
        std::min<size_t>(size - got, static_cast<size_t>(available)));
    if (!::ReadFile(pipe, out + got, to_read, &read, nullptr)) {
      return false;
    }
    if (read == 0) {
      return false;
    }
    got += read;
  }

  return true;
}

bool send_frame(HANDLE pipe, const bison::buffer& frame) {
  const uint32_t size = static_cast<uint32_t>(frame.size());
  if (!write_all(pipe, reinterpret_cast<const uint8_t*>(&size), sizeof(size))) {
    return false;
  }
  if (frame.empty()) {
    return true;
  }
  return write_all(pipe, frame.data(), frame.size());
}

bool recv_frame(
    HANDLE pipe,
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  std::array<uint8_t, sizeof(uint32_t)> header{};
  if (!read_exact(pipe, header.data(), header.size(), timeout)) {
    return false;
  }

  uint32_t size = 0;
  std::memcpy(&size, header.data(), sizeof(size));
  frame.resize(size);
  if (size == 0) {
    return true;
  }

  return read_exact(pipe, frame.data(), frame.size(), timeout);
}

} // namespace

struct pipe_client_transport::impl {
  std::string pipe_name;
  HANDLE pipe = INVALID_HANDLE_VALUE;
  bool opened = false;
};

struct pipe_server_connection::impl {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  bool closed = false;
};

struct pipe_server_transport::impl {
  std::string pipe_name;
  bool running = false;
};

pipe_client_transport::pipe_client_transport(std::string pipe_name)
    : impl_(std::make_unique<impl>()) {
  impl_->pipe_name = std::move(pipe_name);
}

pipe_client_transport::~pipe_client_transport() {
  shutdown();
}

pipe_client_transport::pipe_client_transport(pipe_client_transport&&) noexcept =
    default;
pipe_client_transport& pipe_client_transport::operator=(
    pipe_client_transport&&) noexcept = default;

void pipe_client_transport::open(bison::dynamic&& params) {
  impl_->pipe_name = read_pipe_name(params, impl_->pipe_name);
  shutdown();

  if (!::WaitNamedPipeA(impl_->pipe_name.c_str(), 5000)) {
    throw std::runtime_error("pipe_client_transport::open WaitNamedPipe failed");
  }

  HANDLE pipe = ::CreateFileA(
      impl_->pipe_name.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      0,
      nullptr);

  if (pipe == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("pipe_client_transport::open CreateFile failed");
  }

  DWORD mode = PIPE_READMODE_BYTE;
  ::SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

  impl_->pipe = pipe;
  impl_->opened = true;
}

void pipe_client_transport::send(bison::buffer frame) {
  if (!impl_->opened || impl_->pipe == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("pipe_client_transport::send pipe not open");
  }
  if (!send_frame(impl_->pipe, frame)) {
    throw std::runtime_error("pipe_client_transport::send failed");
  }
}

bool pipe_client_transport::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  if (!impl_->opened || impl_->pipe == INVALID_HANDLE_VALUE) {
    return false;
  }
  return recv_frame(impl_->pipe, frame, timeout);
}

void pipe_client_transport::shutdown() {
  if (impl_->pipe != INVALID_HANDLE_VALUE) {
    ::CloseHandle(impl_->pipe);
    impl_->pipe = INVALID_HANDLE_VALUE;
  }
  impl_->opened = false;
}

pipe_server_connection::pipe_server_connection() = default;

pipe_server_connection::pipe_server_connection(std::unique_ptr<impl> impl)
    : impl_(std::move(impl)) {}

pipe_server_connection::~pipe_server_connection() {
  close();
}

pipe_server_connection::pipe_server_connection(pipe_server_connection&&) noexcept =
    default;
pipe_server_connection& pipe_server_connection::operator=(
    pipe_server_connection&&) noexcept = default;

void pipe_server_connection::send(bison::buffer frame) {
  if (!impl_ || impl_->closed || impl_->pipe == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("pipe_server_connection::send closed");
  }
  if (!send_frame(impl_->pipe, frame)) {
    throw std::runtime_error("pipe_server_connection::send failed");
  }
}

bool pipe_server_connection::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  if (!impl_ || impl_->closed || impl_->pipe == INVALID_HANDLE_VALUE) {
    return false;
  }
  return recv_frame(impl_->pipe, frame, timeout);
}

void pipe_server_connection::close() {
  if (!impl_ || impl_->closed) {
    return;
  }
  if (impl_->pipe != INVALID_HANDLE_VALUE) {
    ::FlushFileBuffers(impl_->pipe);
    ::DisconnectNamedPipe(impl_->pipe);
    ::CloseHandle(impl_->pipe);
    impl_->pipe = INVALID_HANDLE_VALUE;
  }
  impl_->closed = true;
}

bool pipe_server_connection::is_closed() const {
  return !impl_ || impl_->closed || impl_->pipe == INVALID_HANDLE_VALUE;
}

pipe_server_transport::pipe_server_transport(std::string pipe_name)
    : impl_(std::make_unique<impl>()) {
  impl_->pipe_name = std::move(pipe_name);
}

pipe_server_transport::~pipe_server_transport() {
  stop();
}

pipe_server_transport::pipe_server_transport(pipe_server_transport&&) noexcept =
    default;
pipe_server_transport& pipe_server_transport::operator=(
    pipe_server_transport&&) noexcept = default;

void pipe_server_transport::start(bison::dynamic params) {
  impl_->pipe_name = read_pipe_name(params, impl_->pipe_name);
  impl_->running = true;
}

pipe_client_transport pipe_server_transport::connect() const {
  return pipe_client_transport{impl_->pipe_name};
}

std::optional<pipe_server_connection> pipe_server_transport::accept(
    std::chrono::milliseconds timeout) {
  if (!impl_->running) {
    return std::nullopt;
  }

  HANDLE pipe = ::CreateNamedPipeA(
      impl_->pipe_name.c_str(),
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      PIPE_UNLIMITED_INSTANCES,
      kBufferSize,
      kBufferSize,
      0,
      nullptr);

  if (pipe == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }

  OVERLAPPED ov{};
  ov.hEvent = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (ov.hEvent == nullptr) {
    ::CloseHandle(pipe);
    return std::nullopt;
  }

  BOOL connected = ::ConnectNamedPipe(pipe, &ov);
  if (!connected) {
    const DWORD err = ::GetLastError();
    if (err == ERROR_PIPE_CONNECTED) {
      ::SetEvent(ov.hEvent);
    } else if (err != ERROR_IO_PENDING) {
      ::CloseHandle(ov.hEvent);
      ::CloseHandle(pipe);
      return std::nullopt;
    }
  }

  const DWORD wait_ms =
      timeout.count() < 0 ? 0 : static_cast<DWORD>(timeout.count());
  const DWORD wait_rc = ::WaitForSingleObject(ov.hEvent, wait_ms);
  if (wait_rc != WAIT_OBJECT_0) {
    ::CancelIo(pipe);
    ::CloseHandle(ov.hEvent);
    ::CloseHandle(pipe);
    return std::nullopt;
  }

  ::CloseHandle(ov.hEvent);

  auto conn_impl = std::make_unique<pipe_server_connection::impl>();
  conn_impl->pipe = pipe;
  conn_impl->closed = false;
  return pipe_server_connection{std::move(conn_impl)};
}

void pipe_server_transport::stop() {
  impl_->running = false;
}

#else

struct pipe_client_transport::impl {};
struct pipe_server_connection::impl {};
struct pipe_server_transport::impl {};

pipe_client_transport::pipe_client_transport(std::string)
    : impl_(std::make_unique<impl>()) {}
pipe_client_transport::~pipe_client_transport() = default;
pipe_client_transport::pipe_client_transport(pipe_client_transport&&) noexcept =
    default;
pipe_client_transport& pipe_client_transport::operator=(
    pipe_client_transport&&) noexcept = default;
void pipe_client_transport::open(bison::dynamic&&) {
  throw std::runtime_error("pipe transport is only available on Windows in this build");
}
void pipe_client_transport::send(bison::buffer) {
  throw std::runtime_error("pipe transport is only available on Windows in this build");
}
bool pipe_client_transport::receive(bison::buffer&, std::chrono::milliseconds) {
  return false;
}
void pipe_client_transport::shutdown() {}

pipe_server_connection::pipe_server_connection() : impl_(std::make_unique<impl>()) {}
pipe_server_connection::pipe_server_connection(std::unique_ptr<impl> impl)
    : impl_(std::move(impl)) {}
pipe_server_connection::~pipe_server_connection() = default;
pipe_server_connection::pipe_server_connection(pipe_server_connection&&) noexcept =
    default;
pipe_server_connection& pipe_server_connection::operator=(
    pipe_server_connection&&) noexcept = default;
void pipe_server_connection::send(bison::buffer) {
  throw std::runtime_error("pipe transport is only available on Windows in this build");
}
bool pipe_server_connection::receive(bison::buffer&, std::chrono::milliseconds) {
  return false;
}
void pipe_server_connection::close() {}
bool pipe_server_connection::is_closed() const {
  return true;
}

pipe_server_transport::pipe_server_transport(std::string)
    : impl_(std::make_unique<impl>()) {}
pipe_server_transport::~pipe_server_transport() = default;
pipe_server_transport::pipe_server_transport(pipe_server_transport&&) noexcept =
    default;
pipe_server_transport& pipe_server_transport::operator=(pipe_server_transport&&) noexcept =
    default;
void pipe_server_transport::start(bison::dynamic) {
  throw std::runtime_error("pipe transport is only available on Windows in this build");
}
pipe_client_transport pipe_server_transport::connect() const {
  return pipe_client_transport{};
}
std::optional<pipe_server_connection> pipe_server_transport::accept(
    std::chrono::milliseconds) {
  return std::nullopt;
}
void pipe_server_transport::stop() {}

#endif

} // namespace bdg::bison::rmi::transport
