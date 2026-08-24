// MIT License © 2025 Binary Dice Games
/**
 * @file profiler_client.hpp
 * @brief Client-side recorder that batches trace events and ships them to
 *        the server's `__BisonProfiler` singleton over RMI.
 *
 * `attach_profiling(client&)` instantiates the remote profiler proxy,
 * learns this connection's sequence id, and installs a `client_recorder`
 * as the process-wide active recorder (`profiling::recorder::install`) so
 * `BISON_TRACE_SCOPE`/`BISON_TRACE_INSTANT` calls anywhere in the client
 * process start recording once capture is active on the server.
 *
 * See `src/rmi/server/profiler_service.hpp` for the server side and
 * `src/rmi/DESIGN.md` for the overall design.
 */
#pragma once

#include "src/rmi/client/proxy.hpp"
#include "src/rmi/shared/profiling.hpp"

#include <memory>

namespace bdg::bison::rmi {

class client;

/**
 * @brief Recorder whose `flush_sink()` blocks on `submitTraceBlock` from a
 *        dedicated background thread, and self-corrects its active state
 *        from each response (see `profiler_service::submit_trace_block`).
 */
class client_recorder : public profiling::recorder {
 public:
  client_recorder(proxy::dynamic proxy, int32_t sequence_id);
  ~client_recorder() override;

 protected:
  void flush_sink(bison::buffer encoded_batch) override;

 private:
  proxy::dynamic proxy_;
  int32_t sequence_id_;
};

/**
 * @brief Instantiate the remote profiler singleton, install a
 *        `client_recorder` as the process-wide active recorder, and return
 *        it so the caller controls its lifetime.
 *
 * @param c Connected client.
 * @return The installed recorder, or `nullptr` if the server has no
 *         `__BisonProfiler` class registered (profiling not enabled).
 */
std::shared_ptr<client_recorder> attach_profiling(client& c);

} // namespace bdg::bison::rmi
