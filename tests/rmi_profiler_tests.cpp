// MIT License © 2025 Binary Dice Games
// Tests for the RMI Perfetto profiler: recorder unit tests plus a real
// server/client integration test over memory_server_transport.

#include "src/rmi/rmi.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::shared::constants;
using namespace bdg::bison::rmi::transport;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

static void clearClassRegistry() {
  dynamic::getRegistry().wlock()->clear();
}

/** @brief Unique per-test scratch directory, removed on destruction. */
class scoped_temp_dir {
 public:
  scoped_temp_dir() {
    static std::atomic<uint64_t> counter{0};
    const auto n = counter.fetch_add(1);
    path_ = std::filesystem::temp_directory_path() /
            ("bison_profiler_test_" + std::to_string(n) + "_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }
  ~scoped_temp_dir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& path() const {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

/** @brief Test recorder: `flush_sink` appends batches instead of writing a file. */
class test_recorder : public profiling::recorder {
 public:
  explicit test_recorder(uint32_t trusted_packet_sequence_id) : profiling::recorder(trusted_packet_sequence_id) {}
  ~test_recorder() override {
    stop();
  }

  synchronized<std::vector<buffer>> flushed;

 protected:
  void flush_sink(buffer encoded_batch) override {
    flushed.wlock()->push_back(std::move(encoded_batch));
  }
};

/** @brief Walk a batch of concatenated, length-framed TracePacket bytes. */
size_t count_trace_packets(const buffer& batch) {
  size_t pos = 0;
  size_t count = 0;
  while (pos < batch.size()) {
    uint64_t tag = 0;
    int shift = 0;
    while (pos < batch.size()) {
      const uint8_t byte = batch[pos++];
      tag |= static_cast<uint64_t>(byte & 0x7F) << shift;
      if ((byte & 0x80) == 0)
        break;
      shift += 7;
    }
    uint64_t len = 0;
    shift = 0;
    while (pos < batch.size()) {
      const uint8_t byte = batch[pos++];
      len |= static_cast<uint64_t>(byte & 0x7F) << shift;
      if ((byte & 0x80) == 0)
        break;
      shift += 7;
    }
    pos += len;
    ++count;
  }
  return count;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// recorder: hot-path no-ops while inactive
// ═════════════════════════════════════════════════════════════════════════════

TEST(ProfilingRecorder, InactiveRecorderNeverFlushes) {
  test_recorder rec{1};
  EXPECT_FALSE(rec.capture_active());
  for (int i = 0; i < 100; ++i)
    rec.record_instant("noop");

  // No flush thread is even running; give any stray activity a moment to
  // prove itself absent rather than asserting on a zero-duration race.
  const bool flushed_anything =
      rec.flushed.wait_for(std::chrono::milliseconds{50}, [](std::vector<buffer>& v) { return !v.empty(); });
  EXPECT_FALSE(flushed_anything);
}

// ═════════════════════════════════════════════════════════════════════════════
// recorder: size-threshold flush
// ═════════════════════════════════════════════════════════════════════════════

TEST(ProfilingRecorder, SizeThresholdTriggersPromptFlush) {
  test_recorder rec{1};
  rec.set_capture_active(true);

  // kFlushEventThreshold events (a matched begin/end pair per iteration
  // stays well-formed) is enough to force an immediate flush independent of
  // the periodic timer.
  for (size_t i = 0; i < profiling::recorder::kFlushEventThreshold; ++i)
    rec.record_instant("e");

  const bool flushed =
      rec.flushed.wait_for(std::chrono::milliseconds{2000}, [](std::vector<buffer>& v) { return !v.empty(); });
  ASSERT_TRUE(flushed);

  rec.set_capture_active(false);
}

// ═════════════════════════════════════════════════════════════════════════════
// recorder: time-based flush
// ═════════════════════════════════════════════════════════════════════════════

TEST(ProfilingRecorder, TimeBasedFlushFiresWithoutReachingThreshold) {
  test_recorder rec{1};
  rec.set_capture_active(true);

  rec.record_slice_begin("small_slice");
  rec.record_slice_end();

  // Well under kFlushEventThreshold; only the ~200ms periodic timer should
  // cause this to flush. Generous bound keeps this non-flaky under load.
  const bool flushed =
      rec.flushed.wait_for(std::chrono::milliseconds{2000}, [](std::vector<buffer>& v) { return !v.empty(); });
  ASSERT_TRUE(flushed);

  {
    auto lp = rec.flushed.rlock();
    ASSERT_FALSE(lp->empty());
    // First flush of a never-before-seen track emits a TrackDescriptor
    // packet ahead of the TrackEvent packets.
    EXPECT_GE(count_trace_packets((*lp)[0]), 2u);
  }

  rec.set_capture_active(false);
}

// ═════════════════════════════════════════════════════════════════════════════
// recorder: stop() flushes remaining events and halts the background thread
// ═════════════════════════════════════════════════════════════════════════════

TEST(ProfilingRecorder, DeactivatingFlushesRemainder) {
  test_recorder rec{1};
  rec.set_capture_active(true);
  rec.record_instant("one");
  rec.set_capture_active(false);

  auto lp = rec.flushed.rlock();
  EXPECT_FALSE(lp->empty());
}

// ═════════════════════════════════════════════════════════════════════════════
// End-to-end: server::enable_profiling + attach_profiling over memory
// transport.
// ═════════════════════════════════════════════════════════════════════════════

class RmiProfilerE2E : public ::testing::Test {
 protected:
  memory_server_transport server_transport;
  std::unique_ptr<server> srv;
  scoped_temp_dir temp_dir;

  void SetUp() override {
    clearClassRegistry();
    srv = std::make_unique<server>(server_transport);
    srv->enable_profiling(temp_dir.path());
    srv->listen(dynamic{});
  }

  void TearDown() override {
    if (srv)
      srv->stop();
  }

  client make_client() {
    return client{server_transport.connect()};
  }
};

TEST_F(RmiProfilerE2E, SubmitTraceBlockRejectedBeforeStartCapture) {
  auto c = make_client();
  c.connect();

  auto proxy = c.instantiate(NS_BISON, CLASS_PROFILER).get();
  dynamic params;
  params[FIELD_PROFILER_BYTES] = std::vector<uint8_t>{1, 2, 3};
  params[FIELD_PROFILER_SEQUENCE_ID] = int32_t{1};
  dynamic resp = proxy.call(METHOD_SUBMIT_TRACE_BLOCK, std::move(params)).get();
  EXPECT_FALSE(resp.as<bool>(FIELD_PROFILER_ACCEPTED));
  EXPECT_FALSE(resp.as<bool>(FIELD_PROFILER_ACTIVE));

  c.disconnect();
}

TEST_F(RmiProfilerE2E, StartStopCaptureWritesReadableTraceFile) {
  auto c = make_client();
  c.connect();

  auto proxy = c.instantiate(NS_BISON, CLASS_PROFILER).get();

  dynamic start_params;
  start_params[FIELD_PROFILER_LABEL] = std::string{"e2e"};
  dynamic start_resp = proxy.call(METHOD_START_CAPTURE, std::move(start_params)).get();
  ASSERT_TRUE(start_resp.as<bool>(FIELD_PROFILER_OK));
  const std::string trace_path = start_resp.as<std::string>(FIELD_PROFILER_PATH);
  ASSERT_FALSE(trace_path.empty());

  dynamic active_resp = proxy.call(METHOD_IS_CAPTURE_ACTIVE, dynamic{}).get();
  EXPECT_TRUE(active_resp.as<bool>(FIELD_PROFILER_ACTIVE));

  dynamic session = proxy.call(METHOD_BEGIN_PROFILING_SESSION, dynamic{}).get();
  const int32_t sequence_id = session.as<int32_t>(FIELD_PROFILER_SEQUENCE_ID);
  EXPECT_TRUE(session.as<bool>(FIELD_PROFILER_ACTIVE));

  auto attached_recorder = attach_profiling(c);
  ASSERT_NE(attached_recorder, nullptr);
  attached_recorder->set_capture_active(true);
  attached_recorder->record_slice_begin("client_work");
  attached_recorder->record_slice_end();
  (void)sequence_id;

  // The client's recorder only flushes to the server on its own ~200ms
  // background cadence (see profiling::recorder::kFlushInterval); poll the
  // trace file itself -- the observable effect of that flush actually
  // landing -- rather than a fixed sleep or a server-side flag that has
  // nothing to do with client flush timing.
  bool file_has_bytes = false;
  for (int i = 0; i < 40 && !file_has_bytes; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    std::error_code ec;
    file_has_bytes = std::filesystem::file_size(trace_path, ec) > 0 && !ec;
  }
  ASSERT_TRUE(file_has_bytes);

  dynamic stop_resp = proxy.call(METHOD_STOP_CAPTURE, dynamic{}).get();
  EXPECT_TRUE(stop_resp.as<bool>(FIELD_PROFILER_OK));

  attached_recorder->set_capture_active(false);
  c.disconnect();

  ASSERT_TRUE(std::filesystem::exists(trace_path));
  std::ifstream file(trace_path, std::ios::binary);
  ASSERT_TRUE(file.is_open());
  std::vector<uint8_t> contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  EXPECT_GT(contents.size(), 0u);
  EXPECT_GT(count_trace_packets(contents), 0u);
}

TEST_F(RmiProfilerE2E, StartCaptureIsIdempotentWhileActive) {
  auto c = make_client();
  c.connect();
  auto proxy = c.instantiate(NS_BISON, CLASS_PROFILER).get();

  dynamic p1;
  p1[FIELD_PROFILER_LABEL] = std::string{"first"};
  dynamic r1 = proxy.call(METHOD_START_CAPTURE, std::move(p1)).get();
  ASSERT_TRUE(r1.as<bool>(FIELD_PROFILER_OK));
  const std::string path1 = r1.as<std::string>(FIELD_PROFILER_PATH);

  dynamic p2;
  p2[FIELD_PROFILER_LABEL] = std::string{"second"};
  dynamic r2 = proxy.call(METHOD_START_CAPTURE, std::move(p2)).get();
  ASSERT_TRUE(r2.as<bool>(FIELD_PROFILER_OK));
  EXPECT_EQ(r2.as<std::string>(FIELD_PROFILER_PATH), path1);

  proxy.call(METHOD_STOP_CAPTURE, dynamic{}).get();
  c.disconnect();
}

TEST_F(RmiProfilerE2E, StopCaptureIsIdempotentWhileInactive) {
  auto c = make_client();
  c.connect();
  auto proxy = c.instantiate(NS_BISON, CLASS_PROFILER).get();

  dynamic resp = proxy.call(METHOD_STOP_CAPTURE, dynamic{}).get();
  EXPECT_TRUE(resp.as<bool>(FIELD_PROFILER_OK));

  c.disconnect();
}

// ═════════════════════════════════════════════════════════════════════════════
// standalone::enable_profiling + direct-call capture control (no transport,
// no remote client -- standalone drives its own recorder in-process).
// ═════════════════════════════════════════════════════════════════════════════

TEST(RmiProfilerStandalone, StartCaptureNowWritesReadableTraceFile) {
  clearClassRegistry();
  scoped_temp_dir temp_dir;

  standalone sa;
  sa.enable_profiling(temp_dir.path());
  EXPECT_FALSE(sa.is_capture_active_now());

  ASSERT_TRUE(sa.start_capture_now("standalone_test"));
  EXPECT_TRUE(sa.is_capture_active_now());

  BISON_TRACE_SCOPE("standalone_work");

  // local_recorder flushes on its own periodic cadence; poll for a
  // non-empty trace file rather than sleeping a fixed amount.
  std::filesystem::path trace_file;
  bool file_has_bytes = false;
  for (int i = 0; i < 40 && !file_has_bytes; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    for (auto& entry : std::filesystem::directory_iterator(temp_dir.path())) {
      if (entry.path().extension() != ".perfetto-trace")
        continue;
      trace_file = entry.path();
      std::error_code ec;
      file_has_bytes = std::filesystem::file_size(trace_file, ec) > 0 && !ec;
    }
  }
  ASSERT_TRUE(file_has_bytes);

  sa.stop_capture_now();
  EXPECT_FALSE(sa.is_capture_active_now());

  ASSERT_TRUE(std::filesystem::exists(trace_file));
  EXPECT_GT(std::filesystem::file_size(trace_file), 0u);
}

TEST(RmiProfilerStandalone, StartCaptureNowIsNoOpWithoutEnableProfiling) {
  clearClassRegistry();
  standalone sa;
  EXPECT_FALSE(sa.start_capture_now());
  EXPECT_FALSE(sa.is_capture_active_now());
  sa.stop_capture_now(); // no-op; must not throw or crash.
}
