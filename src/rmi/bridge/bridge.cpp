// MIT License © 2025 Binary Dice Games
/**
 * @file bridge.cpp
 * @brief Implementation of rmi::bridge — downstream-to-upstream multiplexer.
 */
#include "src/rmi/bridge/bridge.hpp"

#include "src/bison/bison.hpp"
#include "src/rmi/shared/constants.hpp"

#include <chrono>
#include <vector>

namespace bdg::bison::rmi {

using namespace shared::constants;

// ── Event-intercepting upstream transport ─────────────────────────────────────

/**
 * Wraps a real client transport and intercepts upstream event frames so the
 * bridge can route them to the correct downstream session.
 *
 * receive() loops on event frames, forwarding each to event_handler_, and
 * only returns a frame to the rmi::client when it is a non-event frame.
 * This means the upstream rmi::client never processes event frames directly;
 * the bridge routes all events through route_event().
 */
class bridge_upstream_transport : public transport::client_transport_iface {
 public:
  bridge_upstream_transport(
      std::unique_ptr<transport::client_transport_iface> inner,
      std::function<void(const shared::envelope&)> event_handler)
      : inner_(std::move(inner)), event_handler_(std::move(event_handler)) {}

  void open(bison::dynamic params) override {
    inner_->open(std::move(params));
  }
  void send(bison::buffer frame) override {
    inner_->send(std::move(frame));
  }
  void shutdown() override {
    inner_->shutdown();
  }

  bool receive(bison::buffer& frame, std::chrono::milliseconds timeout) override {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      auto now = std::chrono::steady_clock::now();
      if (now >= deadline)
        return false;
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      if (!inner_->receive(frame, remaining))
        return false;
      try {
        auto env = shared::envelope::decode(frame);
        if (static_cast<bison::hash_t>(env.kind) == static_cast<bison::hash_t>(KIND_EVENT)) {
          if (event_handler_)
            event_handler_(env);
          continue; // Don't expose event frames to rmi::client.
        }
      } catch (...) {
        // Decoding failed; pass the raw frame through unchanged.
      }
      return true;
    }
  }

 private:
  std::unique_ptr<transport::client_transport_iface> inner_;
  std::function<void(const shared::envelope&)> event_handler_;
};

// ── Constructors / destructor ─────────────────────────────────────────────────

bridge::bridge(
    transport::server_transport_iface& downstream_transport,
    std::unique_ptr<transport::client_transport_iface> upstream_transport,
    bison::dynamic upstream_params)
    : server(downstream_transport),
      upstream_client_(
          std::make_unique<bridge_upstream_transport>(
              std::move(upstream_transport),
              [this](const shared::envelope& env) { route_event(env); })),
      upstream_params_(std::move(upstream_params)) {}

bridge::bridge(
    std::unique_ptr<transport::server_transport_iface> downstream_transport,
    std::unique_ptr<transport::client_transport_iface> upstream_transport,
    bison::dynamic upstream_params)
    : server(std::move(downstream_transport)),
      upstream_client_(
          std::make_unique<bridge_upstream_transport>(
              std::move(upstream_transport),
              [this](const shared::envelope& env) { route_event(env); })),
      upstream_params_(std::move(upstream_params)) {}

bridge::~bridge() {
  // Stop downstream workers before destroying state they reference.
  stop();
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void bridge::start(bison::dynamic downstream_params) {
  if (started_.exchange(true))
    return;
  upstream_client_.connect(upstream_params_);
  listen(std::move(downstream_params));
}

void bridge::stop() {
  if (!started_.exchange(false))
    return;
  server::stop();
  upstream_client_.disconnect();
}

// ── Session lifecycle hooks ───────────────────────────────────────────────────

void bridge::on_session_created(context& ctx) {
  auto ss = std::make_shared<session_state>();
  ss->session_id = ctx.session_id;
  ss->emit_st.wlock()->emit = ctx.emit_event;

  sessions_.wlock()->emplace(ctx.session_id.id, ss);
  on_client_connected(ctx);
}

void bridge::on_session_destroyed(context& ctx) {
  on_client_disconnected(ctx);
  teardown_session(ctx.session_id);
}

void bridge::teardown_session(bison::key_t session_id) {
  std::shared_ptr<session_state> ss;
  {
    auto wp = sessions_.wlock();
    auto it = wp->find(session_id.id);
    if (it == wp->end())
      return;
    ss = std::move(it->second);
    wp->erase(it);
  }
  if (!ss)
    return;

  // Hold the emit_st write lock while marking the session inactive.  Any
  // concurrent route_event call that already holds the lock (i.e. is in the
  // middle of calling emit) will complete before we proceed.  After we
  // release the lock below, route_event will see active==false and
  // short-circuit without touching the (soon-to-be-dangling) conn reference
  // captured by emit.
  {
    auto wp = ss->emit_st.wlock();
    wp->active = false;
    wp->emit = nullptr;
  }

  // Destroy every upstream object this session's requests ever created
  // (including ones created indirectly, e.g. wish's UI template system --
  // see context::put_object()), in one shot: every request relayed on this
  // session's behalf was tagged with session_id as its group (see
  // try_handle_request()), so the upstream server can already tell us
  // exactly what to destroy without the bridge tracking anything itself.
  try {
    upstream_client_.send_request_with_group(OP_DESTROY_GROUP, bison::key_t{0u}, session_id, bison::dynamic{}, true)
        .get();
  } catch (...) {
  }
}

// ── Request relay ────────────────────────────────────────────────────────────

bool bridge::try_handle_request(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn) {
  const bison::key_t op = env.op;
  if (op != OP_INSTANTIATE && op != OP_SET && op != OP_GET && op != OP_CALL && op != OP_CLEAR && op != OP_DESTROY) {
    // connect / describe / disconnect / dictionary / help: no upstream
    // object is involved, let the base server handle these locally.
    return false;
  }

  // Tag every relayed request with this downstream session's own ID as its
  // group, so the upstream server files any object created while handling
  // it (directly or indirectly -- see context::put_object()) under that
  // group. This is what lets teardown_session() clean up everything a
  // session ever created in one OP_DESTROY_GROUP request, without the
  // bridge needing to track object IDs itself.
  try {
    bison::dynamic result =
        upstream_client_.send_request_with_group(op, env.object_id, ctx.session_id, env.payload.clone(), env.oneway)
            .get();
    send_response(ctx, conn, env, op, std::move(result));
  } catch (const std::exception& e) {
    send_error(ctx, conn, env, op, ERR_INTERNAL_ERROR, e.what());
  }
  return true;
}

// ── Event routing ─────────────────────────────────────────────────────────────

void bridge::route_event(const shared::envelope& env) {
  // Deliver to the bridge's own upstream-client handler table, for
  // bridge-owned UI (e.g. a desktop compositor widget registered via
  // upstream() in on_client_connected()).
  upstream_client_.dispatch_local_event(env);

  // No per-object ownership tracking: broadcast to every connected
  // downstream session. Each session's own event-handler lookup (already
  // unconditional -- see client::process_frame's KIND_EVENT handling) will
  // silently discard this if it has no handler registered for
  // env.object_id. An object ID is a routing tag here, not a capability, so
  // this is safe even though the bridge doesn't know (and doesn't need to
  // know) which session actually owns the object.
  bison::key_t event_name{0u};
  const auto* name_field = env.payload.findField(FIELD_NAME);
  if (name_field && name_field->is<bison::key_t>())
    event_name = name_field->as<bison::key_t>();

  bison::dynamic params;
  const auto* params_field = env.payload.findField(FIELD_PARAMS);
  if (params_field && params_field->is<bison::dynamic_ptr>()) {
    if (auto ptr = params_field->as<bison::dynamic_ptr>())
      params = ptr->clone();
  }

  std::vector<std::shared_ptr<session_state>> targets;
  {
    auto lp = sessions_.rlock();
    targets.reserve(lp->size());
    for (auto& [id, ss] : *lp)
      targets.push_back(ss);
  }

  for (auto& ss : targets) {
    auto lp = ss->emit_st.wlock();
    if (!lp->active || !lp->emit)
      continue;
    lp->emit(env.object_id, event_name, params.clone());
  }
}

} // namespace bdg::bison::rmi
