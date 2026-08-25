// MIT License © 2025 Binary Dice Games
/**
 * @file profiler_service.hpp
 * @brief Process-wide RMI singleton that owns the server's Perfetto trace
 *        file and receives batched trace blocks from connected clients.
 *
 * A server opts in via `server::enable_profiling(output_dir)`, which
 * registers the `__BisonProfiler` class under `constants::NS_BISON` and
 * installs a `server_local_recorder` (see `profiling::recorder`) for the
 * server's own native code to record into via `BISON_TRACE_SCOPE`. Once
 * registered, `server::on_create_object` hands back this single shared
 * instance for every client that instantiates `__BisonProfiler` -- there is
 * exactly one profiler and one open trace file per server process.
 *
 * See `src/rmi/DESIGN.md` for the client-buffers/server-writes design.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/shared/profiling.hpp"

#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

namespace bdg::bison::rmi {

/**
 * @brief Singleton `dynamic` object exposing profiler control/data RMI
 *        methods and owning the open trace file.
 */
class profiler_service : public bison::dynamic {
 public:
  /**
   * @param base       Base `dynamic` constructed via `dynamic::instantiate`
   *                    against the registered `__BisonProfiler` prototype.
   * @param output_dir Directory `start_capture` writes trace files into.
   *                    Never derived from client input.
   */
  profiler_service(bison::dynamic&& base, std::filesystem::path output_dir);

  ~profiler_service() override;

  // ── RMI method bodies ─────────────────────────────────────────────────
  // Dispatched via addMethod lambdas in register_profiler_class(); public
  // so those lambdas (free functions) can call them on `self`.

  bison::dynamic begin_profiling_session();
  bison::dynamic is_capture_active() const;
  bison::dynamic start_capture(const std::string& label);
  bison::dynamic stop_capture();
  bison::dynamic submit_trace_block(std::vector<uint8_t> bytes, int32_t sequence_id);

 private:
  /** @brief Recorder for the server process's own native code. */
  class server_local_recorder : public profiling::recorder {
   public:
    explicit server_local_recorder(profiler_service& owner) : profiling::recorder(0), owner_(owner) {}
    ~server_local_recorder() override {
      stop();
    }

   protected:
    void flush_sink(bison::buffer encoded_batch) override {
      owner_.append_to_file(encoded_batch);
    }

   private:
    profiler_service& owner_;
  };

  void append_to_file(const bison::buffer& encoded_batch);

  /**
   * @brief Queue a client-submitted trace block for the writer thread, or
   *        drop it if the queue is already at `kMaxQueuedTraceBlocks`.
   *
   * @return `false` if the block was dropped (writer thread can't keep up
   *         with the producer), `true` if it was queued.
   */
  bool enqueue_trace_block(std::vector<uint8_t> bytes);

  /**
   * @brief Body of `writer_thread_`: lowers its own scheduling priority,
   *        then repeatedly drains `pending_blocks_` into the trace file
   *        until `writer_stopping_` is set.
   */
  void writer_loop();

  /** @brief Pop and write every block currently in `pending_blocks_`. */
  void write_pending_blocks();

  struct state {
    std::ofstream file;
    std::filesystem::path current_path;
  };

  /**
   * @brief Upper bound on not-yet-written blocks held in `pending_blocks_`.
   *
   * Bounds memory use against a producer that outpaces the disk -- once
   * full, `enqueue_trace_block` drops new blocks rather than growing the
   * queue or blocking the caller (the RMI dispatch thread).
   */
  static constexpr size_t kMaxQueuedTraceBlocks = 256;

  std::filesystem::path output_dir_;
  std::atomic<bool> capture_active_{false};
  std::atomic<int32_t> next_sequence_id_{1};
  bison::synchronized<state> state_;

  /** @brief Trace blocks submitted by clients, awaiting `writer_thread_`. */
  bison::synchronized<std::deque<std::vector<uint8_t>>> pending_blocks_;
  /**
   * @brief Below-normal-priority thread that performs all file writes for
   *        client-submitted blocks, so `submit_trace_block` (called on an
   *        RMI dispatch thread, see `server::dispatch_worker_state`) never
   *        blocks on file I/O.
   */
  std::thread writer_thread_;
  std::atomic<bool> writer_stopping_{false};

  server_local_recorder local_recorder_;
};

/**
 * @brief Register the `__BisonProfiler` class prototype and construct the
 *        one process-wide `profiler_service` instance.
 *
 * Idempotent-per-call-site: intended to be called exactly once, from
 * `server::enable_profiling()`.
 *
 * @param output_dir Directory trace files are written into.
 * @return The constructed singleton, to be stashed on the server and
 *         returned from its `on_create_object` override.
 */
std::shared_ptr<profiler_service> register_profiler_class(std::filesystem::path output_dir);

} // namespace bdg::bison::rmi
