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
 * `record_counter` (`BISON_TRACE_COUNTER`) records the value-over-time of a
 * named counter track instead of a per-thread slice/instant -- all samples
 * for one counter name share a single track regardless of which thread
 * records them. Counter tracks and their samples still flow through the
 * same buffer/flush/sink pipeline as slices and instants.
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
 *
 * `counter_int_value`/`counter_double_value`/`counter_is_double` are only
 * meaningful when `type == event_type::counter`; slices and instants leave
 * them at their defaults.
 */
struct raw_event {
  uint64_t timestamp_ns = 0;
  uint64_t track_uuid = 0;
  const char* name = "";
  bison::perfetto::event_type type = bison::perfetto::event_type::unspecified;
  int64_t counter_int_value = 0;
  double counter_double_value = 0.0;
  bool counter_is_double = false;
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

  /**
   * @brief Record a new integer sample for the named counter track.
   *
   * Unlike slices/instants, a counter is not tied to the calling thread --
   * all samples recorded under the same @p name land on one shared counter
   * track, so its value-over-time renders as a single row in the Perfetto
   * UI regardless of which thread updates it. @p name is resolved to a
   * `track_uuid` via a locked map keyed on the string contents (not the
   * pointer), so, unlike `record_slice_begin`/`record_instant`, this is not
   * allocation-free on a not-yet-seen name -- acceptable since counters are
   * typically updated far less often than slices/instants.
   */
  void record_counter(const char* name, int64_t value) {
    if (!capture_active_.load(std::memory_order_relaxed))
      return;
    raw_event evt;
    evt.timestamp_ns = now_ns();
    evt.track_uuid = counter_track_uuid(name);
    evt.type = bison::perfetto::event_type::counter;
    evt.counter_int_value = value;
    push(evt);
  }

  /** @brief Record a new floating-point sample for the named counter track. */
  void record_counter(const char* name, double value) {
    if (!capture_active_.load(std::memory_order_relaxed))
      return;
    raw_event evt;
    evt.timestamp_ns = now_ns();
    evt.track_uuid = counter_track_uuid(name);
    evt.type = bison::perfetto::event_type::counter;
    evt.counter_double_value = value;
    evt.counter_is_double = true;
    push(evt);
  }

  /** @brief Set the unit shown for a counter track's values in the Perfetto UI. */
  void set_counter_unit(const char* name, bison::perfetto::counter_unit unit) {
    auto lp = counter_units_.wlock();
    (*lp)[counter_track_uuid(name)] = unit;
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

  /**
   * @brief Resolve a counter's name to a stable `track_uuid`, allocating one
   *        (and registering its name/kind) on first use.
   *
   * Counter track uuids live in a namespace disjoint from
   * `thread_track_uuid()`'s: the latter counts up from 1 in
   * `next_thread_index_`, while counters count up from 1 in
   * `next_counter_index_` with `kCounterTrackFlag` set, so the two schemes
   * can never collide for any process short of ~2 billion threads or
   * counters.
   */
  uint64_t counter_track_uuid(const char* name) {
    {
      auto lp = counter_track_uuids_.rlock();
      auto it = lp->find(name);
      if (it != lp->end())
        return it->second;
    }
    auto lp = counter_track_uuids_.wlock();
    auto it = lp->find(name);
    if (it != lp->end())
      return it->second;
    const uint32_t index = static_cast<uint32_t>(next_counter_index_.fetch_add(1, std::memory_order_relaxed));
    const uint64_t uuid = (static_cast<uint64_t>(trusted_packet_sequence_id_) << 32) | kCounterTrackFlag | index;
    (*lp)[name] = uuid;
    lp.unlock();
    auto names = track_names_.wlock();
    (*names)[uuid] = name;
    return uuid;
  }

  static uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
  }

  void record(bison::perfetto::event_type type, const char* name) {
    if (!capture_active_.load(std::memory_order_relaxed))
      return;
    push(raw_event{now_ns(), thread_track_uuid(), name, type});
  }

  void push(const raw_event& evt) {
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
      const bool is_counter = evt.type == bison::perfetto::event_type::counter;
      if (emitted_tracks_.insert(evt.track_uuid).second) {
        std::string track_name;
        {
          auto lp = track_names_.rlock();
          auto it = lp->find(evt.track_uuid);
          if (it != lp->end())
            track_name = it->second;
        }
        bison::buffer descriptor;
        if (is_counter) {
          // Best-effort: reflects whatever unit was set via
          // set_counter_unit() by the time this, the first sample on this
          // track, reaches the flush thread -- a later set_counter_unit()
          // call does not retroactively rewrite an already-emitted
          // TrackDescriptor.
          bison::perfetto::counter_unit unit = bison::perfetto::counter_unit::unspecified;
          auto units = counter_units_.rlock();
          auto uit = units->find(evt.track_uuid);
          if (uit != units->end())
            unit = uit->second;
          descriptor = bison::perfetto::encode_counter_track_descriptor_packet(
              evt.timestamp_ns, trusted_packet_sequence_id_, evt.track_uuid, track_name, unit);
        } else {
          descriptor = bison::perfetto::encode_track_descriptor_packet(
              evt.timestamp_ns, trusted_packet_sequence_id_, evt.track_uuid, track_name);
        }
        encoded.insert(encoded.end(), descriptor.begin(), descriptor.end());
      }
      bison::buffer packet;
      if (is_counter) {
        packet = evt.counter_is_double
                     ? bison::perfetto::encode_double_counter_event_packet(evt.timestamp_ns,
                           trusted_packet_sequence_id_, evt.track_uuid, evt.counter_double_value)
                     : bison::perfetto::encode_counter_event_packet(evt.timestamp_ns, trusted_packet_sequence_id_,
                           evt.track_uuid, evt.counter_int_value);
      } else {
        packet = bison::perfetto::encode_track_event_packet(
            evt.timestamp_ns, trusted_packet_sequence_id_, evt.track_uuid, evt.type, evt.name);
      }
      encoded.insert(encoded.end(), packet.begin(), packet.end());
    }
    if (!encoded.empty())
      flush_sink(std::move(encoded));
  }

  /** @brief Set in a counter track_uuid's low 32 bits to keep it disjoint from thread indices. */
  static constexpr uint64_t kCounterTrackFlag = uint64_t{1} << 31;

  const uint32_t trusted_packet_sequence_id_;
  std::atomic<bool> capture_active_{false};
  std::atomic<uint64_t> next_thread_index_{1};
  std::atomic<uint64_t> next_counter_index_{1};
  bison::synchronized<std::vector<raw_event>> buffer_;
  bison::synchronized<std::unordered_map<uint64_t, std::string>> track_names_;
  bison::synchronized<std::unordered_map<std::string, uint64_t>> counter_track_uuids_;
  bison::synchronized<std::unordered_map<uint64_t, bison::perfetto::counter_unit>> counter_units_;
  std::unordered_set<uint64_t> emitted_tracks_;
  std::thread flush_thread_;
  std::atomic<bool> stopping_{false};
  static inline std::atomic<recorder*> active_{nullptr};
};

/** @brief RAII helper: records a slice-begin on construction, slice-end on destruction. */
class scoped_slice {
 public:
  explicit scoped_slice(const char* name) {
    enabled_ = (name != nullptr);
    if (enabled_)
      if (recorder* r = recorder::local())
        r->record_slice_begin(name);
  }
  ~scoped_slice() {
    if (enabled_)
      if (recorder* r = recorder::local())
        r->record_slice_end();
  }
  scoped_slice(const scoped_slice&) = delete;
  scoped_slice& operator=(const scoped_slice&) = delete;

 private:
  bool enabled_ = false;
};

/** @brief Record a zero-duration instant event on the process-wide active recorder, if any. */
inline void record_instant(const char* name) {
  if (recorder* r = recorder::local())
    r->record_instant(name);
}

/** @brief Record an integer counter sample on the process-wide active recorder, if any. */
inline void record_counter(const char* name, int64_t value) {
  if (recorder* r = recorder::local())
    r->record_counter(name, value);
}

/** @brief Record a floating-point counter sample on the process-wide active recorder, if any. */
inline void record_counter(const char* name, double value) {
  if (recorder* r = recorder::local())
    r->record_counter(name, value);
}

} // namespace bdg::bison::rmi::profiling

/** @brief Scope-lifetime trace slice; no-op if no recorder is installed/active. */
#define BISON_TRACE_SCOPE(name) ::bdg::bison::rmi::profiling::scoped_slice _bison_trace_scope_##__LINE__ { name }

/** @brief Zero-duration trace event; no-op if no recorder is installed/active. */
#define BISON_TRACE_INSTANT(name) ::bdg::bison::rmi::profiling::record_instant(name)

/**
 * @brief Record a new sample for the named counter track (int64_t or
 * double); no-op if no recorder is installed/active.
 */
#define BISON_TRACE_COUNTER(name, value) ::bdg::bison::rmi::profiling::record_counter(name, value)
