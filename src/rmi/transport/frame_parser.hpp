// MIT License © 2025 Binary Dice Games
/**
 * @file frame_parser.hpp
 * @brief Incremental length-prefixed frame reassembly, shared by every
 *        libuv-backed transport that receives a byte stream.
 *
 * Parses the `[4-byte BE length][payload]` frame format used by every
 * libuv-backed transport (see FORMAT.md §5.1) out of an arbitrary sequence of
 * `feed()` calls, each carrying however many bytes happened to arrive in one
 * read. `uv_stream_state<Handle>` feeds it raw socket bytes directly;
 * `tls_stream_state<Handle>` (see tls_stream_state.hpp) feeds it decrypted
 * TLS application data instead -- the state machine itself has no knowledge
 * of where its bytes come from.
 */
#pragma once

#include "src/bison/bison.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace bdg::bison::rmi::transport {

/**
 * @brief Upper bound on a single frame's declared payload length.
 *
 * The 4-byte length prefix (FORMAT.md §5.1) is attacker/corruption
 * controlled and read before any payload bytes have arrived, so `feed()`
 * checks it against this ceiling before reserving space for it -- without
 * this, a bogus or malicious length prefix (up to ~4 GiB) could drive a
 * single huge allocation per connection. 64 MiB comfortably covers any
 * legitimate bison payload while bounding the worst case.
 */
inline constexpr uint32_t kMaxFrameBytes = 64u * 1024 * 1024;

/**
 * @brief Incremental parser for the 4-byte-BE-length-prefixed frame format.
 *
 * Not thread-safe: callers must confine `feed()`/`offer_reuse()` calls to a
 * single thread (the owning connection's I/O loop thread), matching every
 * existing user of this parser.
 */
struct frame_parser {
  uint8_t hdr[4]{};
  uint32_t hdr_pos{0};
  uint32_t payload_left{0};
  bison::buffer partial;

  /**
   * @brief Offer a previously-used buffer for the parser to reuse instead of
   *        allocating fresh capacity for the next frame's payload.
   *
   * `feed()` cannot keep `partial`'s own allocation across frames -- once a
   * frame completes, ownership of its bytes moves out to `on_frame`'s
   * caller. Callers that later discard a fully-consumed frame buffer can
   * hand it back here (see `uv_stream_state::dequeue_frame()`) so the next
   * frame's `partial.reserve()` reuses that capacity instead of a fresh
   * malloc. No-op if @p buf is not larger than the parser's current scratch
   * buffer.
   */
  void offer_reuse(bison::buffer&& buf) {
    if (buf.capacity() > partial.capacity()) {
      buf.clear();
      partial = std::move(buf);
    }
  }

  /**
   * @brief Feed @p left bytes of newly-arrived application data.
   *
   * Invokes @p on_frame once per complete frame extracted from the
   * accumulated byte stream -- zero, one, or multiple times per call,
   * depending on how frame boundaries line up with @p left.
   *
   * @param p        Start of the newly-arrived bytes.
   * @param left     Number of bytes available at @p p.
   * @param on_frame Called as `on_frame(bison::buffer&& frame)` once per
   *                 complete frame.
   * @return `false` if a declared frame length exceeded `kMaxFrameBytes`
   *         (the caller should treat this as a fatal protocol error and
   *         close the connection); `true` otherwise.
   */
  template <typename OnFrame>
  bool feed(const uint8_t* p, size_t left, OnFrame&& on_frame) {
    while (left > 0) {
      if (hdr_pos < 4) {
        const size_t take = std::min(size_t{4} - hdr_pos, left);
        std::memcpy(hdr + hdr_pos, p, take);
        hdr_pos += static_cast<uint32_t>(take);
        p += take;
        left -= take;
        if (hdr_pos == 4) {
          uint32_t net_hdr{};
          std::memcpy(&net_hdr, hdr, 4);
          payload_left = byte_swap(net_hdr);
          if (payload_left > kMaxFrameBytes)
            return false;
          partial.clear();
          partial.reserve(payload_left);
        }
      }
      if (hdr_pos == 4 && (left > 0 || payload_left == 0)) {
        const size_t take = std::min(static_cast<size_t>(payload_left), left);
        partial.insert(partial.end(), p, p + take);
        payload_left -= static_cast<uint32_t>(take);
        p += take;
        left -= take;
        if (payload_left == 0) {
          on_frame(std::move(partial));
          partial.clear();
          hdr_pos = 0;
        }
      }
    }
    return true;
  }
};

} // namespace bdg::bison::rmi::transport
