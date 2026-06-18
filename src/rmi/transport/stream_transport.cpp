// MIT License © 2025 Binary Dice Games
/**
 * @file stream_transport.cpp
 * @brief iostream-backed transport implementation.
 */
#include "src/rmi/transport/stream_transport.hpp"

#include <stdexcept>
#include <thread>

namespace bdg::bison::rmi::transport {

// ── Framing ───────────────────────────────────────────────────────────────────

// 4-byte big-endian (network byte order) length prefix.
static constexpr std::size_t kLenBytes = 4;

void stream_write_frame(std::ostream& out, const bison::buffer& frame) {
  const uint32_t len_wire =
      bison::byte_swap(static_cast<uint32_t>(frame.size()));
  out.write(reinterpret_cast<const char*>(&len_wire), kLenBytes);
  if (!frame.empty())
    out.write(reinterpret_cast<const char*>(frame.data()),
              static_cast<std::streamsize>(frame.size()));
  out.flush();
  if (out.fail())
    throw std::runtime_error("stream_transport: write failed");
}

bool stream_read_frame(std::istream& in, bison::buffer& frame) {
  uint32_t len_wire{};
  in.read(reinterpret_cast<char*>(&len_wire), kLenBytes);
  if (in.gcount() == 0 && in.eof())
    return false;
  if (in.gcount() != static_cast<std::streamsize>(kLenBytes))
    throw std::runtime_error("stream_transport: partial length prefix");

  const uint32_t len = bison::byte_swap(len_wire);

  frame.resize(len);
  if (len > 0) {
    in.read(reinterpret_cast<char*>(frame.data()),
            static_cast<std::streamsize>(len));
    if (in.gcount() != static_cast<std::streamsize>(len))
      throw std::runtime_error("stream_transport: partial frame payload");
  }
  return true;
}

// ── stream_client_transport ───────────────────────────────────────────────────

stream_client_transport::stream_client_transport(std::iostream& stream)
    : stream_(stream) {}

void stream_client_transport::open(bison::dynamic /*params*/) {}

void stream_client_transport::send(bison::buffer frame) {
  std::lock_guard<std::mutex> lk(send_mtx_);
  stream_write_frame(stream_, frame);
}

bool stream_client_transport::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::lock_guard<std::mutex> lk(recv_mtx_);
  while (!closed_.load()) {
    if (stream_.rdbuf()->in_avail() > 0 || stream_.peek() != std::istream::traits_type::eof()) {
      // Data may be available; attempt a read.
      auto pos = stream_.tellg();
      (void)pos; // not all streams support seeking — just attempt.
      try {
        return stream_read_frame(stream_, frame);
      } catch (...) {
        return false;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    // Yield briefly to avoid spinning when no data is present.
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return false;
}

void stream_client_transport::shutdown() {
  closed_.store(true);
}

// ── stream_server_connection ──────────────────────────────────────────────────

stream_server_connection::stream_server_connection(std::iostream& stream)
    : stream_(stream) {}

void stream_server_connection::send(bison::buffer frame) {
  std::lock_guard<std::mutex> lk(send_mtx_);
  stream_write_frame(stream_, frame);
}

bool stream_server_connection::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::lock_guard<std::mutex> lk(recv_mtx_);
  while (!closed_.load()) {
    if (stream_.rdbuf()->in_avail() > 0 || stream_.peek() != std::istream::traits_type::eof()) {
      try {
        return stream_read_frame(stream_, frame);
      } catch (...) {
        return false;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return false;
}

void stream_server_connection::close() {
  closed_.store(true);
}

bool stream_server_connection::is_closed() const {
  return closed_.load();
}

// ── stream_server_transport ───────────────────────────────────────────────────

stream_server_transport::stream_server_transport(std::iostream& stream)
    : stream_(stream) {}

void stream_server_transport::start(bison::dynamic /*params*/) {
  stopped_.store(false);
}

std::unique_ptr<server_connection_iface> stream_server_transport::accept(
    std::chrono::milliseconds /*timeout*/) {
  if (stopped_.load() || accepted_.exchange(true))
    return nullptr;
  return std::make_unique<stream_server_connection>(stream_);
}

void stream_server_transport::stop() {
  stopped_.store(true);
}

} // namespace bdg::bison::rmi::transport
