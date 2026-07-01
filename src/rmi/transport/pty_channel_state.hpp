// MIT License © 2025 Binary Dice Games
/**
 * @file pty_channel_state.hpp
 * @brief Internal DCS channel state shared by pty_server_transport and
 *        pty_client_transport platform implementations.
 *
 * This header is an implementation detail — it is not part of the public API.
 * Both platform .cpp files include it to get the common state struct and the
 * blocking dequeue helper.
 */
#pragma once

#if defined(__linux__) || defined(_WIN32)

#include "src/bison/bison.hpp"
#include "src/rmi/transport/dcs_framing.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

namespace bdg {
namespace bison {
namespace rmi {
namespace transport {

/**
 * @brief Per-connection state consumed by the dcs_framing template helpers.
 *
 * Fields must match the names expected by push_inbound, close_and_notify,
 * handle_data_frame, process_body, emit_dcs, and emit_data in dcs_framing.hpp.
 */
struct pty_channel_state {
  // ── Receive side ──────────────────────────────────────────────────────────
  std::mutex read_mtx;
  std::queue<bison::buffer> inbox;
  std::condition_variable read_cv;

  // ── Send side ─────────────────────────────────────────────────────────────
  std::mutex write_mtx;
  std::atomic<uint64_t> next_msg_id{0};
  size_t max_chunk_bytes{32UL * 1024};
  size_t max_frame_bytes{64UL * 1024 * 1024};

  // ── Protocol state ────────────────────────────────────────────────────────
  std::atomic<bool> hello_seen{false};
  std::atomic<bool> closed{false};
  std::unordered_map<uint64_t, dcs::partial_message> pending;
  std::chrono::milliseconds reassembly_timeout{std::chrono::seconds{30}};

  // ── Background I/O thread ─────────────────────────────────────────────────
  std::thread read_thread;
  std::atomic<bool> stopped{false};

  pty_channel_state() = default;
  ~pty_channel_state() {
    stopped.store(true);
    if (read_thread.joinable())
      read_thread.join();
  }
  pty_channel_state(const pty_channel_state&) = delete;
  pty_channel_state& operator=(const pty_channel_state&) = delete;

  /**
   * @brief Block until a complete DCS frame is available or the timeout elapses.
   * @param frame   Populated on success.
   * @param timeout Maximum wait duration.
   * @return `true` if a frame was placed in @p frame; `false` on timeout/close.
   */
  bool dequeue(bison::buffer& frame, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lk(read_mtx);
    while (inbox.empty() && !closed.load()) {
      if (read_cv.wait_until(lk, deadline) == std::cv_status::timeout)
        return false;
    }
    if (!inbox.empty()) {
      frame = std::move(inbox.front());
      inbox.pop();
      return true;
    }
    return false;
  }
};

} // namespace transport
} // namespace rmi
} // namespace bison
} // namespace bdg

#endif // defined(__linux__) || defined(_WIN32)
