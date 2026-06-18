// MIT License © 2025 Binary Dice Games
/**
 * @file stream_transport.hpp
 * @brief Transport implementation backed by a std::iostream (e.g. pipe, file,
 *        stringstream).
 *
 * Frames are delimited by a 4-byte big-endian (network byte order) length
 * prefix followed by
 * the frame payload bytes.  Both `stream_client_transport` and
 * `stream_server_connection` wrap a single `std::iostream` reference and are
 * therefore suitable for full-duplex byte channels (pipes, serial ports,
 * socketstreams) as well as loopback streams in tests.
 *
 * @note The caller owns the stream and must ensure it outlives the transport
 *       object.  Thread-safety of the stream itself is the caller's
 *       responsibility; the transport serializes its own send/receive calls
 *       with an internal mutex.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>

namespace bdg::bison::rmi::transport {

// ── Framing helpers (internal linkage in the .cpp) ───────────────────────────

/**
 * @brief Write a length-prefixed frame to an output stream.
 * @param out  Destination stream.
 * @param frame  Payload bytes to write.
 * @throws std::runtime_error on write failure.
 */
void stream_write_frame(std::ostream& out, const bison::buffer& frame);

/**
 * @brief Read one length-prefixed frame from an input stream.
 * @param in  Source stream.
 * @param frame  Output frame buffer.
 * @return `true` on success; `false` if the stream reached EOF before a
 *         complete frame was available.
 * @throws std::runtime_error on a partial read or malformed length prefix.
 */
bool stream_read_frame(std::istream& in, bison::buffer& frame);

// ── Client-side transport ─────────────────────────────────────────────────────

/**
 * @brief Client transport that operates over a `std::iostream`.
 *
 * `open()` is a no-op; the stream must already be connected/open before the
 * object is constructed.  `receive()` blocks until the timeout elapses — it
 * polls the stream because standard C++ streams provide no native wait
 * primitive.
 */
class stream_client_transport : public client_transport_iface {
 public:
  /**
   * @brief Construct from an existing, open iostream.
   * @param stream  Full-duplex byte channel; must outlive this object.
   */
  explicit stream_client_transport(std::iostream& stream);

  /** @brief No-op; the stream must be open before construction. */
  void open(bison::dynamic params) override;

  /**
   * @brief Write one length-prefixed frame to the stream.
   * @throws std::runtime_error on write failure.
   */
  void send(bison::buffer frame) override;

  /**
   * @brief Poll the stream until a full frame arrives or the timeout elapses.
   * @param frame   Output frame buffer.
   * @param timeout Maximum wait before returning `false`.
   * @return `true` when a frame was received; `false` on timeout or EOF.
   */
  bool receive(
      bison::buffer& frame,
      std::chrono::milliseconds timeout =
          std::chrono::milliseconds{5000}) override;

  /** @brief Mark the transport as shut down; subsequent receives return false. */
  void shutdown() override;

 private:
  std::iostream& stream_;
  std::mutex send_mtx_;
  std::mutex recv_mtx_;
  std::atomic<bool> closed_{false};
};

// ── Server-side connection ────────────────────────────────────────────────────

/**
 * @brief Server-side connection backed by a `std::iostream`.
 *
 * Mirrors `stream_client_transport` from the server perspective.  Typically
 * produced by `stream_server_transport::accept()`.
 */
class stream_server_connection : public server_connection_iface {
 public:
  /**
   * @brief Construct from an existing, open iostream.
   * @param stream  Full-duplex byte channel; must outlive this object.
   */
  explicit stream_server_connection(std::iostream& stream);

  /**
   * @brief Write one length-prefixed frame to the stream.
   * @throws std::runtime_error on write failure.
   */
  void send(bison::buffer frame) override;

  /**
   * @brief Poll the stream until a full frame arrives or the timeout elapses.
   * @param frame   Output frame buffer.
   * @param timeout Maximum wait before returning `false`.
   * @return `true` when a frame was received; `false` on timeout or EOF.
   */
  bool receive(
      bison::buffer& frame,
      std::chrono::milliseconds timeout =
          std::chrono::milliseconds{5000}) override;

  /** @brief Mark the connection closed; subsequent receives return false. */
  void close() override;

  /** @brief Return whether `close()` has been called. */
  bool is_closed() const override;

 private:
  std::iostream& stream_;
  std::mutex send_mtx_;
  std::mutex recv_mtx_;
  std::atomic<bool> closed_{false};
};

// ── Server-side transport (single-stream listener) ────────────────────────────

/**
 * @brief Server transport that accepts a single pre-connected iostream.
 *
 * Intended for embeddings where exactly one stream is available (e.g. stdin/
 * stdout pipe, a loopback stringstream in tests).  `accept()` returns the one
 * connection on the first call and `nullptr` on every subsequent call.
 *
 * For multi-client scenarios, construct individual `stream_server_connection`
 * objects directly rather than going through this class.
 */
class stream_server_transport : public server_transport_iface {
 public:
  /**
   * @brief Construct with the pre-connected stream to vend on the first
   *        `accept()`.
   * @param stream  Full-duplex byte channel; must outlive this object.
   */
  explicit stream_server_transport(std::iostream& stream);

  stream_server_transport(const stream_server_transport&) = delete;
  stream_server_transport& operator=(const stream_server_transport&) = delete;

  /** @brief No-op; the stream must be open before construction. */
  void start(bison::dynamic params) override;

  /**
   * @brief Return the one connection on the first call; `nullptr` thereafter.
   * @param timeout  Ignored; no blocking is needed for a pre-connected stream.
   */
  std::unique_ptr<server_connection_iface> accept(
      std::chrono::milliseconds timeout =
          std::chrono::milliseconds{5000}) override;

  /** @brief Mark the transport stopped; future accept calls return nullptr. */
  void stop() override;

 private:
  std::iostream& stream_;
  std::atomic<bool> stopped_{false};
  std::atomic<bool> accepted_{false};
};

} // namespace bdg::bison::rmi::transport
