// MIT License © 2025 Binary Dice Games
#include "src/rmi/client/profiler_client.hpp"

#include "src/rmi/client/client.hpp"
#include "src/rmi/shared/constants.hpp"


#include <stdexcept>

namespace bdg::bison::rmi {

using namespace shared::constants;

client_recorder::client_recorder(proxy::dynamic proxy, int32_t sequence_id)
    : profiling::recorder(static_cast<uint32_t>(sequence_id)), proxy_(std::move(proxy)), sequence_id_(sequence_id) {}

client_recorder::~client_recorder() {
  stop();
  if (profiling::recorder::local() == this)
    profiling::recorder::install(nullptr);
}

void client_recorder::flush_sink(bison::buffer encoded_batch) {
  bison::dynamic params;
  params[FIELD_PROFILER_BYTES] = std::move(encoded_batch);
  params[FIELD_PROFILER_SEQUENCE_ID] = sequence_id_;
  try {
    bison::dynamic resp = proxy_.call(METHOD_SUBMIT_TRACE_BLOCK, std::move(params)).get();
    set_capture_active(resp.as<bool>(FIELD_PROFILER_ACTIVE));
  } catch (const std::exception&) {
    // Connection dropped or server rejected the call; stop recording rather
    // than accumulate an unbounded buffer against a dead connection.
    set_capture_active(false);
  }
}

std::shared_ptr<client_recorder> attach_profiling(client& c) {
  try {
    proxy::dynamic proxy = c.instantiate(NS_BISON, CLASS_PROFILER).get();
    bison::dynamic session = proxy.call(METHOD_BEGIN_PROFILING_SESSION, bison::dynamic{}).get();
    const int32_t sequence_id = session.as<int32_t>(FIELD_PROFILER_SEQUENCE_ID);
    auto recorder = std::make_shared<client_recorder>(std::move(proxy), sequence_id);
    recorder->set_capture_active(session.as<bool>(FIELD_PROFILER_ACTIVE));
    profiling::recorder::install(recorder.get());
    return recorder;
  } catch (const std::exception&) {
    return nullptr;
  }
}

} // namespace bdg::bison::rmi
