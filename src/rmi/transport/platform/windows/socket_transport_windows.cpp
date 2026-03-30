// MIT License © 2025 Binary Dice Games
/**
 * @file socket_transport_windows.cpp
 * @brief Windows TCP socket transport implementation.
 */
#include "src/rmi/transport/socket_transport.hpp"

#include <array>
#include <cstring>
#include <mutex>
#include <stdexcept>

#ifdef _WIN32
#include <WS2tcpip.h>
#include <WinSock2.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

namespace bdg::bison::rmi::transport {

#ifdef _WIN32
namespace {

void ensure_winsock_started() {
  static std::once_flag flag;
  std::call_once(flag, [] {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw std::runtime_error("WSAStartup failed");
    }
  });
}

std::string to_port_string(uint16_t port) {
  return std::to_string(static_cast<unsigned>(port));
}

bool send_all(SOCKET sock, const uint8_t* data, size_t size) {
  size_t sent = 0;
  while (sent < size) {
    const int rc = ::send(
        sock,
        reinterpret_cast<const char*>(data + sent),
        static_cast<int>(size - sent),
        0);
    if (rc <= 0) {
      return false;
    }
    sent += static_cast<size_t>(rc);
  }
  return true;
}

bool wait_readable(SOCKET sock, std::chrono::milliseconds timeout) {
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(sock, &read_set);

  timeval tv{};
  tv.tv_sec = static_cast<long>(timeout.count() / 1000);
  tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

  const int rc = ::select(0, &read_set, nullptr, nullptr, &tv);
  return rc > 0 && FD_ISSET(sock, &read_set);
}

bool recv_exact(
    SOCKET sock,
    uint8_t* out,
    size_t size,
    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  size_t got = 0;

  while (got < size) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return false;
    }

    const auto remain =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (!wait_readable(sock, remain)) {
      return false;
    }

    const int rc = ::recv(
        sock,
        reinterpret_cast<char*>(out + got),
        static_cast<int>(size - got),
        0);
    if (rc <= 0) {
      return false;
    }
    got += static_cast<size_t>(rc);
  }

  return true;
}

bool send_frame(SOCKET sock, const bison::buffer& frame) {
  const uint32_t sz = htonl(static_cast<uint32_t>(frame.size()));
  if (!send_all(sock, reinterpret_cast<const uint8_t*>(&sz), sizeof(sz))) {
    return false;
  }
  if (frame.empty()) {
    return true;
  }
  return send_all(sock, frame.data(), frame.size());
}

bool recv_frame(
    SOCKET sock,
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  std::array<uint8_t, sizeof(uint32_t)> header{};
  if (!recv_exact(sock, header.data(), header.size(), timeout)) {
    return false;
  }

  uint32_t net_size = 0;
  std::memcpy(&net_size, header.data(), sizeof(net_size));
  const uint32_t size = ntohl(net_size);
  frame.resize(size);

  if (size == 0) {
    return true;
  }
  return recv_exact(sock, frame.data(), frame.size(), timeout);
}

std::string read_string_param(
    const bison::dynamic& params,
    bison::key_t key,
    std::string fallback) {
  if (const auto* f = params.findField(key);
      f != nullptr && f->is<std::string>()) {
    return f->as<std::string>();
  }
  return fallback;
}

uint16_t read_port_param(
    const bison::dynamic& params,
    bison::key_t key,
    uint16_t fallback) {
  if (const auto* f = params.findField(key); f != nullptr) {
    if (f->is<int32_t>()) {
      const auto value = f->as<int32_t>();
      if (value >= 0 && value <= 65535) {
        return static_cast<uint16_t>(value);
      }
    }
  }
  return fallback;
}

} // namespace

struct socket_client_transport::impl {
  std::string host;
  uint16_t port = 7070;
  SOCKET sock = INVALID_SOCKET;
  bool opened = false;
};

struct socket_server_connection::impl {
  SOCKET sock = INVALID_SOCKET;
  bool closed = false;
};

struct socket_server_transport::impl {
  std::string bind_host;
  uint16_t port = 7070;
  SOCKET listen_sock = INVALID_SOCKET;
  bool running = false;
};

socket_client_transport::socket_client_transport(
    std::string host,
    uint16_t port)
    : impl_(std::make_unique<impl>()) {
  impl_->host = std::move(host);
  impl_->port = port;
}

socket_client_transport::~socket_client_transport() {
  shutdown();
}

socket_client_transport::socket_client_transport(
    socket_client_transport&&) noexcept = default;
socket_client_transport& socket_client_transport::operator=(
    socket_client_transport&&) noexcept = default;

void socket_client_transport::open(bison::dynamic&& params) {
  if (!impl_) {
    throw std::runtime_error("socket_client_transport::open invalid state");
  }

  ensure_winsock_started();

  impl_->host = read_string_param(params, "host"_key, impl_->host);
  impl_->host = read_string_param(params, "__host"_key, impl_->host);
  impl_->port = read_port_param(params, "port"_key, impl_->port);
  impl_->port = read_port_param(params, "__port"_key, impl_->port);

  shutdown();

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* result = nullptr;
  const auto port_s = to_port_string(impl_->port);
  const int rc =
      ::getaddrinfo(impl_->host.c_str(), port_s.c_str(), &hints, &result);
  if (rc != 0 || result == nullptr) {
    throw std::runtime_error(
        "socket_client_transport::open getaddrinfo failed");
  }

  SOCKET sock = INVALID_SOCKET;
  for (auto* it = result; it != nullptr; it = it->ai_next) {
    sock = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (sock == INVALID_SOCKET) {
      continue;
    }
    if (::connect(sock, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
      break;
    }
    ::closesocket(sock);
    sock = INVALID_SOCKET;
  }

  ::freeaddrinfo(result);

  if (sock == INVALID_SOCKET) {
    throw std::runtime_error("socket_client_transport::open connect failed");
  }

  impl_->sock = sock;
  impl_->opened = true;
}

void socket_client_transport::send(bison::buffer frame) {
  if (!impl_) {
    throw std::runtime_error("socket_client_transport::send invalid state");
  }
  if (!impl_->opened || impl_->sock == INVALID_SOCKET) {
    throw std::runtime_error("socket_client_transport::send socket not open");
  }
  if (!send_frame(impl_->sock, frame)) {
    throw std::runtime_error("socket_client_transport::send failed");
  }
}

bool socket_client_transport::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  if (!impl_) {
    return false;
  }
  if (!impl_->opened || impl_->sock == INVALID_SOCKET) {
    return false;
  }
  return recv_frame(impl_->sock, frame, timeout);
}

void socket_client_transport::shutdown() {
  if (!impl_) {
    return;
  }
  if (impl_->sock != INVALID_SOCKET) {
    ::shutdown(impl_->sock, SD_BOTH);
    ::closesocket(impl_->sock);
    impl_->sock = INVALID_SOCKET;
  }
  impl_->opened = false;
}

socket_server_connection::socket_server_connection() = default;

socket_server_connection::socket_server_connection(std::unique_ptr<impl> impl)
    : impl_(std::move(impl)) {}

socket_server_connection::~socket_server_connection() {
  close();
}

socket_server_connection::socket_server_connection(
    socket_server_connection&&) noexcept = default;
socket_server_connection& socket_server_connection::operator=(
    socket_server_connection&&) noexcept = default;

void socket_server_connection::send(bison::buffer frame) {
  if (!impl_ || impl_->closed || impl_->sock == INVALID_SOCKET) {
    throw std::runtime_error("socket_server_connection::send closed");
  }
  if (!send_frame(impl_->sock, frame)) {
    throw std::runtime_error("socket_server_connection::send failed");
  }
}

bool socket_server_connection::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  if (!impl_ || impl_->closed || impl_->sock == INVALID_SOCKET) {
    return false;
  }
  return recv_frame(impl_->sock, frame, timeout);
}

void socket_server_connection::close() {
  if (!impl_ || impl_->closed) {
    return;
  }
  if (impl_->sock != INVALID_SOCKET) {
    ::shutdown(impl_->sock, SD_BOTH);
    ::closesocket(impl_->sock);
    impl_->sock = INVALID_SOCKET;
  }
  impl_->closed = true;
}

bool socket_server_connection::is_closed() const {
  return !impl_ || impl_->closed || impl_->sock == INVALID_SOCKET;
}

socket_server_transport::socket_server_transport(
    std::string bind_host,
    uint16_t port)
    : impl_(std::make_unique<impl>()) {
  impl_->bind_host = std::move(bind_host);
  impl_->port = port;
}

socket_server_transport::~socket_server_transport() {
  stop();
}

socket_server_transport::socket_server_transport(
    socket_server_transport&&) noexcept = default;
socket_server_transport& socket_server_transport::operator=(
    socket_server_transport&&) noexcept = default;

void socket_server_transport::start(bison::dynamic params) {
  if (!impl_) {
    throw std::runtime_error("socket_server_transport::start invalid state");
  }

  ensure_winsock_started();

  impl_->bind_host = read_string_param(params, "host"_key, impl_->bind_host);
  impl_->bind_host = read_string_param(params, "__host"_key, impl_->bind_host);
  impl_->port = read_port_param(params, "port"_key, impl_->port);
  impl_->port = read_port_param(params, "__port"_key, impl_->port);

  stop();

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;

  addrinfo* result = nullptr;
  const auto port_s = to_port_string(impl_->port);
  const int rc =
      ::getaddrinfo(impl_->bind_host.c_str(), port_s.c_str(), &hints, &result);
  if (rc != 0 || result == nullptr) {
    throw std::runtime_error(
        "socket_server_transport::start getaddrinfo failed");
  }

  SOCKET listen_sock = INVALID_SOCKET;
  for (auto* it = result; it != nullptr; it = it->ai_next) {
    listen_sock = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (listen_sock == INVALID_SOCKET) {
      continue;
    }

    BOOL reuse = TRUE;
    ::setsockopt(
        listen_sock,
        SOL_SOCKET,
        SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse),
        sizeof(reuse));

    if (::bind(listen_sock, it->ai_addr, static_cast<int>(it->ai_addrlen)) !=
        0) {
      ::closesocket(listen_sock);
      listen_sock = INVALID_SOCKET;
      continue;
    }

    if (::listen(listen_sock, SOMAXCONN) != 0) {
      ::closesocket(listen_sock);
      listen_sock = INVALID_SOCKET;
      continue;
    }

    break;
  }

  ::freeaddrinfo(result);

  if (listen_sock == INVALID_SOCKET) {
    throw std::runtime_error(
        "socket_server_transport::start bind/listen failed");
  }

  impl_->listen_sock = listen_sock;
  impl_->running = true;
}

socket_client_transport socket_server_transport::connect() const {
  if (!impl_) {
    throw std::runtime_error("socket_server_transport::connect invalid state");
  }
  return socket_client_transport{impl_->bind_host, impl_->port};
}

std::optional<socket_server_connection> socket_server_transport::accept(
    std::chrono::milliseconds timeout) {
  if (!impl_) {
    return std::nullopt;
  }
  if (!impl_->running || impl_->listen_sock == INVALID_SOCKET) {
    return std::nullopt;
  }

  if (!wait_readable(impl_->listen_sock, timeout)) {
    return std::nullopt;
  }

  SOCKET client = ::accept(impl_->listen_sock, nullptr, nullptr);
  if (client == INVALID_SOCKET) {
    return std::nullopt;
  }

  auto conn_impl = std::make_unique<socket_server_connection::impl>();
  conn_impl->sock = client;
  conn_impl->closed = false;
  return socket_server_connection{std::move(conn_impl)};
}

void socket_server_transport::stop() {
  if (!impl_) {
    return;
  }
  if (impl_->listen_sock != INVALID_SOCKET) {
    ::closesocket(impl_->listen_sock);
    impl_->listen_sock = INVALID_SOCKET;
  }
  impl_->running = false;
}

#else

struct socket_client_transport::impl {};
struct socket_server_connection::impl {};
struct socket_server_transport::impl {};

socket_client_transport::socket_client_transport(std::string, uint16_t)
    : impl_(std::make_unique<impl>()) {}
socket_client_transport::~socket_client_transport() = default;
socket_client_transport::socket_client_transport(
    socket_client_transport&&) noexcept = default;
socket_client_transport& socket_client_transport::operator=(
    socket_client_transport&&) noexcept = default;
void socket_client_transport::open(bison::dynamic&&) {
  throw std::runtime_error(
      "socket transport is only available on Windows in this build");
}
void socket_client_transport::send(bison::buffer) {
  throw std::runtime_error(
      "socket transport is only available on Windows in this build");
}
bool socket_client_transport::receive(
    bison::buffer&,
    std::chrono::milliseconds) {
  return false;
}
void socket_client_transport::shutdown() {}

socket_server_connection::socket_server_connection()
    : impl_(std::make_unique<impl>()) {}
socket_server_connection::socket_server_connection(std::unique_ptr<impl> impl)
    : impl_(std::move(impl)) {}
socket_server_connection::~socket_server_connection() = default;
socket_server_connection::socket_server_connection(
    socket_server_connection&&) noexcept = default;
socket_server_connection& socket_server_connection::operator=(
    socket_server_connection&&) noexcept = default;
void socket_server_connection::send(bison::buffer) {
  throw std::runtime_error(
      "socket transport is only available on Windows in this build");
}
bool socket_server_connection::receive(
    bison::buffer&,
    std::chrono::milliseconds) {
  return false;
}
void socket_server_connection::close() {}
bool socket_server_connection::is_closed() const {
  return true;
}

socket_server_transport::socket_server_transport(std::string, uint16_t)
    : impl_(std::make_unique<impl>()) {}
socket_server_transport::~socket_server_transport() = default;
socket_server_transport::socket_server_transport(
    socket_server_transport&&) noexcept = default;
socket_server_transport& socket_server_transport::operator=(
    socket_server_transport&&) noexcept = default;
void socket_server_transport::start(bison::dynamic) {
  throw std::runtime_error(
      "socket transport is only available on Windows in this build");
}
socket_client_transport socket_server_transport::connect() const {
  return socket_client_transport{};
}
std::optional<socket_server_connection> socket_server_transport::accept(
    std::chrono::milliseconds) {
  return std::nullopt;
}
void socket_server_transport::stop() {}

#endif

} // namespace bdg::bison::rmi::transport
