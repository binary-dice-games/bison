// MIT License © 2025 Binary Dice Games
#include "src/rmi/server/profiler_service.hpp"

#include "src/rmi/shared/constants.hpp"

using bdg::bison::attr;
using bdg::bison::dynamic;
using bdg::bison::dynamic_ptr;
using bdg::bison::field;

namespace bdg::bison::rmi {

using namespace shared::constants;

profiler_service::profiler_service(bison::dynamic&& base, std::filesystem::path output_dir)
    : bison::dynamic(std::move(base)), output_dir_(std::move(output_dir)), local_recorder_(*this) {
  // Makes the server's own native code (BISON_TRACE_SCOPE / BISON_TRACE_INSTANT)
  // record into this process's trace file, alongside client-submitted blocks.
  profiling::recorder::install(&local_recorder_);
}

profiler_service::~profiler_service() {
  if (profiling::recorder::local() == &local_recorder_)
    profiling::recorder::install(nullptr);
  local_recorder_.stop();
}

void profiler_service::append_to_file(const bison::buffer& encoded_batch) {
  auto lp = state_.wlock();
  if (!lp->file.is_open())
    return;
  lp->file.write(reinterpret_cast<const char*>(encoded_batch.data()), static_cast<std::streamsize>(encoded_batch.size()));
  lp->file.flush();
}

bison::dynamic profiler_service::begin_profiling_session() {
  const int32_t sequence_id = next_sequence_id_.fetch_add(1, std::memory_order_relaxed);
  bison::dynamic resp;
  resp[FIELD_PROFILER_SEQUENCE_ID] = sequence_id;
  resp[FIELD_PROFILER_ACTIVE] = capture_active_.load(std::memory_order_relaxed);
  return resp;
}

bison::dynamic profiler_service::is_capture_active() const {
  bison::dynamic resp;
  resp[FIELD_PROFILER_ACTIVE] = capture_active_.load(std::memory_order_relaxed);
  return resp;
}

bison::dynamic profiler_service::start_capture(const std::string& label) {
  bison::dynamic resp;
  if (capture_active_.load(std::memory_order_relaxed)) {
    auto lp = state_.rlock();
    resp[FIELD_PROFILER_OK] = true;
    resp[FIELD_PROFILER_PATH] = lp->current_path.string();
    return resp;
  }

  std::error_code ec;
  std::filesystem::create_directories(output_dir_, ec);

  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  std::string filename = "trace-" + std::to_string(ms);
  if (!label.empty())
    filename += "-" + label;
  filename += ".perfetto-trace";
  const std::filesystem::path path = output_dir_ / filename;

  {
    auto lp = state_.wlock();
    lp->file.open(path, std::ios::binary | std::ios::trunc);
    if (!lp->file.is_open()) {
      resp[FIELD_PROFILER_OK] = false;
      resp[FIELD_PROFILER_PATH] = std::string{};
      return resp;
    }
    lp->current_path = path;
  }

  capture_active_.store(true, std::memory_order_relaxed);
  local_recorder_.set_capture_active(true);

  resp[FIELD_PROFILER_OK] = true;
  resp[FIELD_PROFILER_PATH] = path.string();
  return resp;
}

bison::dynamic profiler_service::stop_capture() {
  bison::dynamic resp;
  if (!capture_active_.load(std::memory_order_relaxed)) {
    resp[FIELD_PROFILER_OK] = true;
    return resp;
  }

  capture_active_.store(false, std::memory_order_relaxed);
  local_recorder_.set_capture_active(false);

  auto lp = state_.wlock();
  if (lp->file.is_open())
    lp->file.close();

  resp[FIELD_PROFILER_OK] = true;
  return resp;
}

bison::dynamic profiler_service::submit_trace_block(std::vector<uint8_t> bytes, int32_t sequence_id) {
  (void)sequence_id;
  bison::dynamic resp;
  const bool active = capture_active_.load(std::memory_order_relaxed);
  if (!active || bytes.empty()) {
    resp[FIELD_PROFILER_ACCEPTED] = false;
    resp[FIELD_PROFILER_ACTIVE] = active;
    return resp;
  }
  append_to_file(bytes);
  resp[FIELD_PROFILER_ACCEPTED] = true;
  resp[FIELD_PROFILER_ACTIVE] = true;
  return resp;
}

std::shared_ptr<profiler_service> register_profiler_class(std::filesystem::path output_dir) {
  auto proto = dynamic_ptr{CLASS_PROFILER, {}};

  proto->addMethod(METHOD_BEGIN_PROFILING_SESSION,
                    bison::method{[](dynamic& s, const dynamic&) -> dynamic {
                      return static_cast<profiler_service&>(s).begin_profiling_session();
                    }});

  proto->addMethod(METHOD_IS_CAPTURE_ACTIVE,
                    bison::method{[](dynamic& s, const dynamic&) -> dynamic {
                      return static_cast<profiler_service&>(s).is_capture_active();
                    }});

  auto start_in = std::make_shared<dynamic>();
  start_in->addField(FIELD_PROFILER_LABEL, field{std::string{}, attr<DisplayName>("label")});
  proto->addMethod(METHOD_START_CAPTURE,
                    bison::method{
                        [](dynamic& s, const dynamic& p) -> dynamic {
                          return static_cast<profiler_service&>(s).start_capture(
                              p.as<std::string>(FIELD_PROFILER_LABEL));
                        },
                        dynamic_ptr{start_in}, nullptr, attr<DisplayName>("startCapture")});

  proto->addMethod(METHOD_STOP_CAPTURE,
                    bison::method{[](dynamic& s, const dynamic&) -> dynamic {
                      return static_cast<profiler_service&>(s).stop_capture();
                    }});

  auto submit_in = std::make_shared<dynamic>();
  submit_in->addField(FIELD_PROFILER_BYTES, field{std::vector<uint8_t>{}, attr<DisplayName>("bytes")});
  submit_in->addField(FIELD_PROFILER_SEQUENCE_ID, field{int32_t{0}, attr<DisplayName>("sequenceId")});
  proto->addMethod(METHOD_SUBMIT_TRACE_BLOCK,
                    bison::method{
                        [](dynamic& s, const dynamic& p) -> dynamic {
                          return static_cast<profiler_service&>(s).submit_trace_block(
                              p.as<std::vector<uint8_t>>(FIELD_PROFILER_BYTES),
                              p.as<int32_t>(FIELD_PROFILER_SEQUENCE_ID));
                        },
                        dynamic_ptr{submit_in}, nullptr, attr<DisplayName>("submitTraceBlock")});

  dynamic::addClass(NS_BISON, std::move(proto));

  return std::make_shared<profiler_service>(dynamic::instantiate(NS_BISON, CLASS_PROFILER),
                                             std::move(output_dir));
}

} // namespace bdg::bison::rmi
