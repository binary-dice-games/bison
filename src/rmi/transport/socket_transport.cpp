// MIT License © 2025 Binary Dice Games
/**
 * @file socket_transport.cpp
 * @brief Cross-platform TCP socket transport implemented with standalone Asio.
 */
#include "src/rmi/transport/socket_transport.hpp"

#include <asio.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace bdg::bison::rmi::transport {

namespace {

using asio::ip::tcp;

constexpr auto kPollInterval = std::chrono::milliseconds{1};

std::string to_port_string(uint16_t port) {
  return std::to_string(static_cast<unsigned>(port));
}

bool is_retryable_error(const asio::error_code& ec) {
  return ec == asio::error::would_block || ec == asio::error::try_again ||
      ec == asio::error::interrupted;
}

void sleep_until_deadline(std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return;
  }

  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  std::this_thread::sleep_for(std::min(remaining, kPollInterval));
}

bool write_all(tcp::socket& socket, const uint8_t* data, size_t size) {
  size_t sent = 0;
  while (sent < size) {
    asio::error_code ec;
    const auto written =
        socket.write_some(asio::buffer(data + sent, size - sent), ec);
    if (!ec) {
      sent += written;
      continue;
    }
    if (is_retryable_error(ec)) {
      std::this_thread::sleep_for(kPollInterval);
      continue;
    }
    return false;
  }
  return true;
}

bool read_exact(
    tcp::socket& socket,
    uint8_t* out,
    size_t size,
    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  size_t received = 0;

  while (received < size) {
    asio::error_code ec;
    const auto chunk =
        socket.read_some(asio::buffer(out + received, size - received), ec);
    if (!ec) {
      received += chunk;
      continue;
    }
    if (is_retryable_error(ec)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      sleep_until_deadline(deadline);
      continue;
    }
    return false;
  }

  return true;
}

bool send_frame(tcp::socket& socket, const bison::buffer& frame) {
  const uint32_t net_size = htonl(static_cast<uint32_t>(frame.size()));
  if (!write_all(
          socket,
          reinterpret_cast<const uint8_t*>(&net_size),
          sizeof(net_size))) {
    return false;
  }
  if (frame.empty()) {
    return true;
  }
  return write_all(socket, frame.data(), frame.size());
}

bool recv_frame(
    tcp::socket& socket,
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  std::array<uint8_t, sizeof(uint32_t)> header{};
  if (!read_exact(socket, header.data(), header.size(), timeout)) {
    return false;
  }

  uint32_t net_size = 0;
  std::memcpy(&net_size, header.data(), sizeof(net_size));
  const uint32_t size = ntohl(net_size);
  frame.resize(size);

  if (size == 0) {
    return true;
  }
  return read_exact(socket, frame.data(), frame.size(), timeout);
}

std::string read_string_param(
    const bison::dynamic& params,
    bison::key_t key,
    std::string fallback) {
  if (const auto* field = params.findField(key);
      field != nullptr && field->is<std::string>()) {
    return field->as<std::string>();
  }
  return fallback;
}

uint16_t read_port_param(
    const bison::dynamic& params,
    bison::key_t key,
    uint16_t fallback) {
  if (const auto* field = params.findField(key);
      field != nullptr && field->is<int32_t>()) {
    const auto value = field->as<int32_t>();
    if (value >= 0 && value <= 65535) {
      return static_cast<uint16_t>(value);
    }
  }
  return fallback;
}

std::runtime_error make_transport_error(
    const std::string& where,
    const asio::error_code& ec) {
  return std::runtime_error(where + ": " + ec.message());
}

} // namespace

struct socket_client_transport::impl {
  impl() : socket(io_context) {}

  asio::io_context io_context;
  tcp::socket socket;
  std::string host;
  uint16_t port = 7070;
  bool opened = false;
};

struct socket_server_connection::impl {
  impl(std::shared_ptr<asio::io_context> io, tcp::socket accepted_socket)
      : io_context(std::move(io)), socket(std::move(accepted_socket)) {}

  std::shared_ptr<asio::io_context> io_context;
  tcp::socket socket;
  bool closed = false;
};

struct socket_server_transport::impl {
  impl() : acceptor(io_context) {}

  asio::io_context io_context;
  tcp::acceptor acceptor;
  std::string bind_host;
  uint16_t port = 7070;
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

  impl_->host = read_string_param(params, "host"_key, impl_->host);
  impl_->host = read_string_param(params, "__host"_key, impl_->host);
  impl_->port = read_port_param(params, "port"_key, impl_->port);
  impl_->port = read_port_param(params, "__port"_key, impl_->port);

  shutdown();
  impl_->socket = tcp::socket{impl_->io_context};

  asio::error_code ec;
  tcp::resolver resolver(impl_->io_context);
  const auto endpoints =
      resolver.resolve(impl_->host, to_port_string(impl_->port), ec);
  if (ec || endpoints.begin() == endpoints.end()) {
    throw make_transport_error(
        "socket_client_transport::open resolve failed", ec);
  }

  asio::connect(impl_->socket, endpoints, ec);
  if (ec) {
    shutdown();
    throw make_transport_error(
        "socket_client_transport::open connect failed", ec);
  }

  impl_->socket.non_blocking(true, ec);
  if (ec) {
    shutdown();
    throw make_transport_error(
        "socket_client_transport::open non_blocking failed", ec);
  }

  impl_->opened = true;
}

void socket_client_transport::send(bison::buffer frame) {
  if (!impl_) {
    throw std::runtime_error("socket_client_transport::send invalid state");
  }
  if (!impl_->opened || !impl_->socket.is_open()) {
    throw std::runtime_error("socket_client_transport::send socket not open");
  }
  if (!send_frame(impl_->socket, frame)) {
    throw std::runtime_error("socket_client_transport::send failed");
  }
}

bool socket_client_transport::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  if (!impl_) {
    return false;
  }
  if (!impl_->opened || !impl_->socket.is_open()) {
    return false;
  }
  return recv_frame(impl_->socket, frame, timeout);
}

void socket_client_transport::shutdown() {
  if (!impl_) {
    return;
  }

  asio::error_code ec;
  if (impl_->socket.is_open()) {
    impl_->socket.shutdown(tcp::socket::shutdown_both, ec);
    ec.clear();
    impl_->socket.close(ec);
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
  if (!impl_ || impl_->closed || !impl_->socket.is_open()) {
    throw std::runtime_error("socket_server_connection::send closed");
  }
  if (!send_frame(impl_->socket, frame)) {
    throw std::runtime_error("socket_server_connection::send failed");
  }
}

bool socket_server_connection::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  if (!impl_ || impl_->closed || !impl_->socket.is_open()) {
    return false;
  }
  return recv_frame(impl_->socket, frame, timeout);
}

void socket_server_connection::close() {
  if (!impl_ || impl_->closed) {
    return;
  }

  asio::error_code ec;
  if (impl_->socket.is_open()) {
    impl_->socket.shutdown(tcp::socket::shutdown_both, ec);
    ec.clear();
    impl_->socket.close(ec);
  }
  impl_->closed = true;
}

bool socket_server_connection::is_closed() const {
  return !impl_ || impl_->closed || !impl_->socket.is_open();
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

  impl_->bind_host = read_string_param(params, "host"_key, impl_->bind_host);
  impl_->bind_host = read_string_param(params, "__host"_key, impl_->bind_host);
  impl_->port = read_port_param(params, "port"_key, impl_->port);
  impl_->port = read_port_param(params, "__port"_key, impl_->port);

  stop();

  asio::error_code ec;
  tcp::resolver resolver(impl_->io_context);
  const auto endpoints = resolver.resolve(
      impl_->bind_host,
      to_port_string(impl_->port),
      tcp::resolver::passive,
      ec);
  if (ec || endpoints.begin() == endpoints.end()) {
    throw make_transport_error(
        "socket_server_transport::start resolve failed", ec);
  }

  for (const auto& entry : endpoints) {
    impl_->acceptor.open(entry.endpoint().protocol(), ec);
    if (ec) {
      continue;
    }

    impl_->acceptor.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
      impl_->acceptor.close();
      continue;
    }

    impl_->acceptor.bind(entry.endpoint(), ec);
    if (ec) {
      impl_->acceptor.close();
      continue;
    }

    impl_->acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
      impl_->acceptor.close();
      continue;
    }

    impl_->acceptor.non_blocking(true, ec);
    if (ec) {
      impl_->acceptor.close();
      continue;
    }

    impl_->running = true;
    return;
  }

  throw std::runtime_error("socket_server_transport::start bind/listen failed");
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
  if (!impl_->running || !impl_->acceptor.is_open()) {
    return std::nullopt;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() <= deadline) {
    auto io_context = std::make_shared<asio::io_context>();
    tcp::socket client_socket{*io_context};

    asio::error_code ec;
    impl_->acceptor.accept(client_socket, ec);
    if (!ec) {
      client_socket.non_blocking(true, ec);
      if (ec) {
        asio::error_code close_ec;
        client_socket.close(close_ec);
        return std::nullopt;
      }

      auto conn_impl = std::make_unique<socket_server_connection::impl>(
          std::move(io_context), std::move(client_socket));
      conn_impl->closed = false;
      return socket_server_connection{std::move(conn_impl)};
    }

    if (ec == asio::error::operation_aborted ||
        ec == asio::error::bad_descriptor) {
      return std::nullopt;
    }
    if (!is_retryable_error(ec)) {
      return std::nullopt;
    }

    sleep_until_deadline(deadline);
  }

  return std::nullopt;
}

void socket_server_transport::stop() {
  if (!impl_) {
    return;
  }

  asio::error_code ec;
  impl_->acceptor.cancel(ec);
  ec.clear();
  if (impl_->acceptor.is_open()) {
    impl_->acceptor.close(ec);
  }
  impl_->running = false;
}

} // namespace bdg::bison::rmi::transport