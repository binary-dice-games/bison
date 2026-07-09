// MIT License © 2025 Binary Dice Games
/**
 * @file bridge.cpp
 * @brief Implementation of rmi::bridge — downstream-to-upstream multiplexer.
 */
#include "src/rmi/bridge/bridge.hpp"

#include "src/bison/bison.hpp"
#include "src/rmi/shared/constants.hpp"
#include "src/rmi/shared/ids.hpp"

#include <chrono>
#include <sstream>
#include <stdexcept>

namespace bdg::bison::rmi {

using namespace shared::constants;

// ── Thread-local storage ──────────────────────────────────────────────────────

thread_local bison::key_t bridge::current_request_id_{0u};
thread_local bridge::pending_clear_state bridge::pending_clear_{};

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
    transport::server_transport_iface& downstream,
    std::unique_ptr<transport::client_transport_iface> upstream_transport,
    bison::dynamic upstream_params)
    : server(downstream),
      upstream_client_(
          std::make_unique<bridge_upstream_transport>(
              std::move(upstream_transport),
              [this](const shared::envelope& env) { route_event(env); })),
      upstream_params_(std::move(upstream_params)) {}

bridge::bridge(
    std::unique_ptr<transport::server_transport_iface> downstream,
    std::unique_ptr<transport::client_transport_iface> upstream_transport,
    bison::dynamic upstream_params)
    : server(std::move(downstream)),
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
  uint32_t idx = ns_counter_.fetch_add(1u);
  // Unique key for this session's logical namespace.
  bison::key_t ns_prefix{static_cast<bison::hash_t>(0xBD6E0000u | idx)};

  auto ss = std::make_shared<session_state>();
  ss->session_id = ctx.session_id;
  ss->ns_prefix = ns_prefix;
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
  // Hold the emit_st write lock while marking the session inactive.  Any
  // concurrent route_event call that already holds the lock (i.e. is in the
  // middle of calling emit) will complete before we proceed.  After we
  // release the lock below, route_event will see active==false and
  // short-circuit without touching the (soon-to-be-dangling) conn reference
  // captured by emit.
  if (ss) {
    auto wp = ss->emit_st.wlock();
    wp->active = false;
    wp->emit = nullptr;
  }
}

// ── Helper ────────────────────────────────────────────────────────────────────

std::shared_ptr<bridge::session_state> bridge::find_session(bison::key_t id) const {
  auto lp = sessions_.rlock();
  auto it = lp->find(id.id);
  if (it == lp->end())
    return nullptr;
  return it->second;
}

// ── Proxy construction ────────────────────────────────────────────────────────

bison::dynamic_ptr bridge::make_proxy_obj(
    bison::key_t upstream_oid,
    std::weak_ptr<session_state> ws,
    std::shared_ptr<bison::key_t> local_oid_slot) {
  auto proxy = std::make_shared<bison::dynamic>();

  // HOOK_SETTER: forward OP_SET upstream; return {} so handle_set applies
  // no local field changes.
  proxy->addMethod(
      bison::key_t{HOOK_SETTER},
      bison::method{[this, upstream_oid](bison::dynamic& /*self*/, const bison::dynamic& patch) {
        try {
          upstream_client_.send_request(OP_SET, upstream_oid, patch.clone(), false).get();
        } catch (...) {
        }
        return bison::dynamic{};
      }});

  // HOOK_GETTER: fetch the full state from upstream and return it as the
  // GET response, overriding handle_get's locally-built snapshot.
  proxy->addMethod(
      bison::key_t{HOOK_GETTER},
      bison::method{[this, upstream_oid](bison::dynamic& /*self*/, const bison::dynamic& /*snap*/) {
        try {
          return upstream_client_.send_request(OP_GET, upstream_oid, bison::dynamic{}, false).get();
        } catch (...) {
          return bison::dynamic{};
        }
      }});

  // CALL_FALLBACK: handle_call builds wrapper = { "__name": name,
  // "__params": dynamic_ptr{args} }, which is exactly the OP_CALL payload
  // format.  Forward it directly to upstream.
  proxy->addMethod(
      bison::key_t{bison::dynamic::CALL_FALLBACK},
      bison::method{[this, upstream_oid](bison::dynamic& /*self*/, const bison::dynamic& wrapper) {
        return upstream_client_.send_request(OP_CALL, upstream_oid, wrapper.clone(), false).get();
      }});

  // HOOK_DESTRUCT: called by handle_destroy (explicit destroy) and by
  // cleanup_context during session teardown.  Always destroys the upstream
  // object.  Cleans up translation tables while the session is still alive.
  proxy->addMethod(
      bison::key_t{HOOK_DESTRUCT},
      bison::method{[this, upstream_oid, ws, local_oid_slot](bison::dynamic& /*self*/, const bison::dynamic& /*p*/) {
        // Remove from the global event routing table unconditionally.
        upstream_to_session_.wlock()->erase(upstream_oid.id);

        // Clean up per-session translation tables (no-op after teardown).
        if (auto ss = ws.lock()) {
          const bison::key_t local_oid = *local_oid_slot;
          if (local_oid.id != 0u) {
            ss->local_to_upstream.wlock()->erase(local_oid.id);
            ss->upstream_to_local.wlock()->erase(upstream_oid.id);
          }
        }

        // Destroy the upstream object (fire-and-forget oneway).
        try {
          upstream_client_.send_request(OP_DESTROY, upstream_oid, bison::dynamic{}, true).get();
        } catch (...) {
        }

        return bison::dynamic{};
      }});

  return proxy;
}

// ── on_check_class ────────────────────────────────────────────────────────────

bool bridge::on_check_class(context& /*ctx*/, bison::key_t /*ns*/, bison::key_t /*klass*/) {
  // The bridge accepts any class and forwards instantiation to the upstream
  // server.  No local registry check is needed.
  return true;
}

// ── on_create_object ─────────────────────────────────────────────────────────

bison::dynamic_ptr bridge::on_create_object(context& ctx, bison::key_t ns, bison::key_t klass) {
  auto ss = find_session(ctx.session_id);
  if (!ss) {
    throw std::runtime_error("bridge: session not found in on_create_object");
  }

  // Instantiate the object on the upstream server.  This blocks the server
  // worker thread while the upstream client's worker thread processes the
  // response.
  proxy::dynamic upstream_proxy = upstream_client_.instantiate(ns, klass).get();
  bison::key_t upstream_oid = upstream_proxy.object_id();
  // Drop upstream_proxy: ~proxy::dynamic() is default and does NOT send
  // OP_DESTROY.  The bridge proxy's HOOK_DESTRUCT sends it instead.

  auto local_oid_slot = std::make_shared<bison::key_t>(bison::key_t{0u});

  // Stash a pending relay keyed by the current envelope's request_id.
  // on_response_trace will consume it to finalise the local↔upstream mapping.
  {
    auto wp = pending_relays_.wlock();
    (*wp)[current_request_id_.id] = pending_relay{upstream_oid, ctx.session_id, local_oid_slot};
  }

  return make_proxy_obj(upstream_oid, std::weak_ptr<session_state>{ss}, local_oid_slot);
}

// ── on_request_trace ─────────────────────────────────────────────────────────

void bridge::on_request_trace(context& ctx, const shared::envelope& env) {
  // Save the request_id for on_create_object to key pending_relays_.
  current_request_id_ = env.request_id;

  // OP_CLEAR: handle_clear resets the local proxy object (destroying its hooks)
  // before on_response_trace fires.  Forward the clear to upstream now, then
  // stash state so on_response_trace can re-install a fresh proxy afterward.
  if (static_cast<bison::hash_t>(env.op) == static_cast<bison::hash_t>(OP_CLEAR)) {
    pending_clear_ = {};
    auto ss = find_session(ctx.session_id);
    if (ss) {
      const bison::key_t local_oid = env.object_id;
      bison::key_t upstream_oid{0u};
      {
        auto lp = ss->local_to_upstream.rlock();
        auto it = lp->find(local_oid.id);
        if (it != lp->end())
          upstream_oid = it->second;
      }
      if (upstream_oid.id != 0u) {
        try {
          upstream_client_.send_request(OP_CLEAR, upstream_oid, bison::dynamic{}, false).get();
        } catch (...) {
        }
        pending_clear_.active = true;
        pending_clear_.local_oid = local_oid;
        pending_clear_.upstream_oid = upstream_oid;
        pending_clear_.session_id = ctx.session_id;
      }
    }
  }
}

// ── on_response_trace ────────────────────────────────────────────────────────

void bridge::on_response_trace(
    context& ctx,
    const shared::envelope& request_env,
    bison::key_t op,
    bool is_error,
    bison::key_t /*error_code*/,
    const bison::dynamic& response_payload) {
  // ── OP_INSTANTIATE: finalise local↔upstream mapping ─────────────────────
  if (static_cast<bison::hash_t>(op) == static_cast<bison::hash_t>(OP_INSTANTIATE) && !is_error) {
    pending_relay relay;
    {
      auto wp = pending_relays_.wlock();
      auto it = wp->find(request_env.request_id.id);
      if (it == wp->end())
        return;
      relay = std::move(it->second);
      wp->erase(it);
    }

    const auto* oid_field = response_payload.findField(FIELD_OBJECT_ID);
    if (!oid_field || !oid_field->is<bison::key_t>())
      return;
    const bison::key_t local_oid = oid_field->as<bison::key_t>();

    // Fill the shared slot so HOOK_DESTRUCT can remove table entries later.
    *relay.local_oid_slot = local_oid;

    auto ss = find_session(relay.session_id);
    if (ss) {
      ss->local_to_upstream.wlock()->emplace(local_oid.id, relay.upstream_oid);
      ss->upstream_to_local.wlock()->emplace(relay.upstream_oid.id, local_oid);
    }

    // Register the global reverse lookup so route_event can find this object.
    upstream_to_session_.wlock()->emplace(relay.upstream_oid.id, std::make_pair(relay.session_id, local_oid));
  }

  // ── OP_CLEAR: re-install the proxy that handle_clear destroyed ───────────
  if (static_cast<bison::hash_t>(op) == static_cast<bison::hash_t>(OP_CLEAR) && !is_error && pending_clear_.active) {
    const auto pcs = pending_clear_;
    pending_clear_ = {};

    auto ss = find_session(pcs.session_id);
    if (ss) {
      auto slot = std::make_shared<bison::key_t>(pcs.local_oid);
      auto new_proxy = make_proxy_obj(pcs.upstream_oid, std::weak_ptr<session_state>{ss}, std::move(slot));
      ctx.objects[pcs.local_oid.id] = std::move(new_proxy);
    }
  }
}

// ── Event routing ─────────────────────────────────────────────────────────────

void bridge::route_event(const shared::envelope& env) {
  const bison::key_t upstream_oid = env.object_id;

  // Extract event name and parameters from the envelope payload.
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

  // Locate the downstream session that owns this upstream object (O(1)).
  bison::key_t session_id;
  bison::key_t local_oid;
  {
    auto lp = upstream_to_session_.rlock();
    auto it = lp->find(upstream_oid.id);
    if (it == lp->end()) {
      // No downstream session owns this object -- it was instantiated by
      // the bridge itself (e.g. a subclass building its own upstream UI).
      // Deliver it through the upstream client's own handler table instead
      // of dropping it.
      upstream_client_.dispatch_local_event(env);
      return;
    }
    session_id = it->second.first;
    local_oid = it->second.second;
  }

  // Resolve the session and call emit under the emit_st lock.  This prevents
  // a teardown_session call (which sets active=false) from racing with our
  // emit call: either we finish before teardown or teardown sets
  // active=false first and we skip the call entirely.
  std::shared_ptr<session_state> ss;
  {
    auto lp = sessions_.rlock();
    auto it = lp->find(session_id.id);
    if (it == lp->end())
      return;
    ss = it->second;
  }

  {
    auto lp = ss->emit_st.wlock();
    if (!lp->active || !lp->emit)
      return;
    lp->emit(local_oid, event_name, std::move(params));
  }
}

} // namespace bdg::bison::rmi
