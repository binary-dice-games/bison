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
 * @brief Incremental parser for the 4-byte-BE-length-prefixed frame format.
 *
 * Not thread-safe: callers must confine `feed()` calls to a single thread
 * (the owning connection's I/O loop thread), matching every existing user of
 * this parser.
 */
struct frame_parser {
  uint8_t hdr[4]{};
  uint32_t hdr_pos{0};
  uint32_t payload_left{0};
  bison::buffer partial;

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
   */
  template <typename OnFrame>
  void feed(const uint8_t* p, size_t left, OnFrame&& on_frame) {
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
          partial = bison::buffer{};
          hdr_pos = 0;
        }
      }
    }
  }
};

} // namespace bdg::bison::rmi::transport
