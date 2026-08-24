// MIT License © 2025 Binary Dice Games
/**
 * @file profiling.hpp
 * @brief Shared hot-path event recording and batching core for bison's
 *        Perfetto-format RMI profiler.
 *
 * `recorder` is the piece both `rmi::profiler_client` (batches and ships
 * events to the server over RMI) and `rmi::profiler_service`'s
 * `server_local_recorder` (writes straight to the open trace file) build
 * on. The hot path (`record_slice_begin`/`record_slice_end`/
 * `record_instant`) is a single relaxed atomic load plus, when capture is
 * active, a lock + push_back into an in-memory buffer -- no allocation, no
 * protobuf encoding, no I/O. Encoding and I/O both happen off the hot path,
 * in a background flush thread woken by a size or time threshold.
 *
 * See `src/bison/bison_perfetto.hpp` for the wire-format encoder this
 * module drives, and `src/rmi/DESIGN.md` for the overall client-buffers/
 * server-writes architecture.
 */
#pragma once

#include "src/bison/bison_perfetto.hpp"
#include "src/bison/bison_sync.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bdg::bison::rmi::profiling {

/**
 * @brief One recorded event, captured on the hot path.
 *
 * `name` must point at a string literal or otherwise static-storage-
 * duration string -- the hot path never copies or allocates it, only the
 * background flush thread reads through the pointer when encoding.
 */
struct raw_event {
  uint64_t timestamp_ns = 0;
  uint64_t track_uuid = 0;
  const char* name = "";
  bison::perfetto::event_type type = bison::perfetto::event_type::unspecified;
};

/**
 * @brief Abstract base: buffers recorded events and periodically hands a
 *        fully-encoded batch to `flush_sink()`.
 *
 * Exactly one `recorder` is "installed" per process at a time (see
 * `install()`/`local()`) -- the macros below always operate on that one
 * instance, so C++ code using `BISON_TRACE_SCOPE` never has to thread a
 * recorder pointer through call sites.
 */
class recorder {
 public:
  /** @brief Size threshold that triggers an immediate flush. */
  static constexpr size_t kFlushEventThreshold = 2048;
  /** @brief Time threshold that triggers a flush even below the size threshold. */
  static constexpr std::chrono::milliseconds kFlushInterval{200};

  explicit recorder(uint32_t trusted_packet_sequence_id) : trusted_packet_sequence_id_(trusted_packet_sequence_id) {}

  /**
   * @brief Base destructor safety net: stops the flush thread WITHOUT
   *        flushing (a final flush would call the now-invalid, already-
   *        destroyed derived `flush_sink()` override).
   *
   * Every subclass must call `stop()` at the top of its own destructor,
   * while its `flush_sink()` override is still valid, so the final flush
   * actually happens.
   */
  virtual ~recorder() {
    stopping_.store(true, std::memory_order_relaxed);
    buffer_.notify_all();
    if (flush_thread_.joinable())
      flush_thread_.join();
  }

  recorder(const recorder&) = delete;
  recorder& operator=(const recorder&) = delete;

  /**
   * @brief Flush remaining events and stop the background flush thread.
   *
   * Subclasses must call this at the top of their own destructor (before
   * any of their own state is torn down) so the final flush's call to
   * `flush_sink()` is still valid.
   */
  void stop() {
    set_capture_active(false);
  }

  // ── Hot path ────────────────────────────────────────────────────────────

  /** @brief Record the start of a named slice on the calling thread's track. */
  void record_slice_begin(const char* name) {
    record(bison::perfetto::event_type::slice_begin, name);
  }

  /** @brief Record the end of the most recently begun slice on this thread's track. */
  void record_slice_end() {
    record(bison::perfetto::event_type::slice_end, "");
  }

  /** @brief Record a zero-duration instant event on the calling thread's track. */
  void record_instant(const char* name) {
    record(bison::perfetto::event_type::instant, name);
  }

  // ── Lifecycle ───────────────────────────────────────────────────────────

  /**
   * @brief Enable or disable recording and start/stop the background flush
   *        thread accordingly.
   *
   * Disabling drops any buffered-but-unflushed events after one final
   * flush attempt -- acceptable for a profiler (best-effort tail loss on
   * stop is documented, not a correctness issue).
   */
  void set_capture_active(bool active) {
    if (active == capture_active_.load(std::memory_order_relaxed))
      return;
    capture_active_.store(active, std::memory_order_relaxed);
    if (active) {
      start_flush_thread();
    } else {
      flush_once();
      stop_flush_thread();
    }
  }

  bool capture_active() const {
    return capture_active_.load(std::memory_order_relaxed);
  }

  /** @brief Optional human-readable name for the calling thread's track. */
  void set_thread_track_name(std::string name) {
    tls_state().track_name = std::move(name);
  }

  // ── Process-wide active recorder ───────────────────────────────────────

  static recorder* local() {
    return active_.load(std::memory_order_acquire);
  }

  static void install(recorder* r) {
    active_.store(r, std::memory_order_release);
  }

 protected:
  /**
   * @brief Deliver one already-encoded batch of `TracePacket`s (raw
   *        concatenated bytes, ready to append to a trace file).
   *
   * Called on the background flush thread only. Implementations should not
   * block indefinitely -- a slow sink delays the next flush cycle but
   * never blocks the hot path, which never touches this method.
   */
  virtual void flush_sink(bison::buffer encoded_batch) = 0;

 private:
  struct thread_local_state {
    uint64_t track_uuid = 0;
    std::string track_name;
  };

  thread_local_state& tls_state() {
    thread_local thread_local_state state;
    return state;
  }

  uint64_t thread_track_uuid() {
    auto& state = tls_state();
    if (state.track_uuid == 0) {
      const uint32_t index = static_cast<uint32_t>(next_thread_index_.fetch_add(1, std::memory_order_relaxed));
      state.track_uuid = (static_cast<uint64_t>(trusted_packet_sequence_id_) << 32) | index;
      if (state.track_name.empty()) {
        std::ostringstream oss;
        oss << "thread-" << std::this_thread::get_id();
        state.track_name = oss.str();
      }
      auto lp = track_names_.wlock();
      (*lp)[state.track_uuid] = state.track_name;
    }
    return state.track_uuid;
  }

  static uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
  }

  void record(bison::perfetto::event_type type, const char* name) {
    if (!capture_active_.load(std::memory_order_relaxed))
      return;
    raw_event evt{now_ns(), thread_track_uuid(), name, type};
    auto lp = buffer_.wlock();
    lp->push_back(evt);
    const bool should_flush = lp->size() >= kFlushEventThreshold;
    lp.unlock();
    if (should_flush)
      buffer_.notify_one();
  }

  void start_flush_thread() {
    stopping_.store(false, std::memory_order_relaxed);
    flush_thread_ = std::thread([this] { flush_loop(); });
  }

  void stop_flush_thread() {
    stopping_.store(true, std::memory_order_relaxed);
    buffer_.notify_all();
    if (flush_thread_.joinable())
      flush_thread_.join();
  }

  void flush_loop() {
    while (!stopping_.load(std::memory_order_relaxed)) {
      buffer_.wait_for(kFlushInterval, [this](std::vector<raw_event>& buf) {
        return buf.size() >= kFlushEventThreshold || stopping_.load(std::memory_order_relaxed);
      });
      flush_once();
    }
  }

  void flush_once() {
    std::vector<raw_event> batch;
    {
      auto lp = buffer_.wlock();
      if (lp->empty())
        return;
      batch.swap(*lp);
    }
    bison::buffer encoded;
    for (const raw_event& evt : batch) {
      if (emitted_tracks_.insert(evt.track_uuid).second) {
        std::string track_name;
        {
          auto lp = track_names_.rlock();
          auto it = lp->find(evt.track_uuid);
          if (it != lp->end())
            track_name = it->second;
        }
        auto descriptor = bison::perfetto::encode_track_descriptor_packet(
            evt.timestamp_ns, trusted_packet_sequence_id_, evt.track_uuid, track_name);
        encoded.insert(encoded.end(), descriptor.begin(), descriptor.end());
      }
      auto packet = bison::perfetto::encode_track_event_packet(
          evt.timestamp_ns, trusted_packet_sequence_id_, evt.track_uuid, evt.type, evt.name);
      encoded.insert(encoded.end(), packet.begin(), packet.end());
    }
    if (!encoded.empty())
      flush_sink(std::move(encoded));
  }

  const uint32_t trusted_packet_sequence_id_;
  std::atomic<bool> capture_active_{false};
  std::atomic<uint64_t> next_thread_index_{1};
  bison::synchronized<std::vector<raw_event>> buffer_;
  bison::synchronized<std::unordered_map<uint64_t, std::string>> track_names_;
  std::unordered_set<uint64_t> emitted_tracks_;
  std::thread flush_thread_;
  std::atomic<bool> stopping_{false};
  static inline std::atomic<recorder*> active_{nullptr};
};

/** @brief RAII helper: records a slice-begin on construction, slice-end on destruction. */
class scoped_slice {
 public:
  explicit scoped_slice(const char* name) {
    if (recorder* r = recorder::local())
      r->record_slice_begin(name);
  }
  ~scoped_slice() {
    if (recorder* r = recorder::local())
      r->record_slice_end();
  }
  scoped_slice(const scoped_slice&) = delete;
  scoped_slice& operator=(const scoped_slice&) = delete;
};

/** @brief Record a zero-duration instant event on the process-wide active recorder, if any. */
inline void record_instant(const char* name) {
  if (recorder* r = recorder::local())
    r->record_instant(name);
}

} // namespace bdg::bison::rmi::profiling

/** @brief Scope-lifetime trace slice; no-op if no recorder is installed/active. */
#define BISON_TRACE_SCOPE(name) ::bdg::bison::rmi::profiling::scoped_slice _bison_trace_scope_##__LINE__ { name }

/** @brief Zero-duration trace event; no-op if no recorder is installed/active. */
#define BISON_TRACE_INSTANT(name) ::bdg::bison::rmi::profiling::record_instant(name)
