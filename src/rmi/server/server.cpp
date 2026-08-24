// MIT License © 2025 Binary Dice Games
/**
 * @file server.cpp
 * @brief Implementation of the RMI server accept loop and request handlers.
 */
#include "src/rmi/server/server.hpp"

#include "src/bison/bison_print.hpp"
#include "src/rmi/shared/constants.hpp"
#include "src/rmi/shared/ids.hpp"
#include "src/rmi/shared/schemas.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace bdg::bison::rmi {

using namespace shared::constants;
using namespace transport;

// ── Trace helpers ─────────────────────────────────────────────────────────────

namespace {

static const char* op_to_label(bison::key_t op) {
  static const std::unordered_map<bison::key_t, const char*, bison::key_t, bison::key_t> labels = {
      {OP_CONNECT, "connect    "},
      {OP_DISCONNECT, "disconnect "},
      {OP_INSTANTIATE, "instantiate"},
      {OP_CALL, "call       "},
      {OP_GET, "get        "},
      {OP_SET, "set        "},
      {OP_DESTROY, "destroy    "},
      {OP_DESTROY_GROUP, "destroyGrp "},
      {OP_CLEAR, "clear      "},
      {OP_DESCRIBE, "describe   "},
      {OP_DICTIONARY, "dictionary "},
      {OP_HELP, "help       "},
  };

  auto it = labels.find(op);
  return it != labels.end() ? it->second : "unknown    ";
}

// Reads the live `_rkey` / `register_key_name` registry on every call, so a
// class/method registered after an earlier trace line still resolves.
static std::string resolve(bison::hash_t h) {
  if (auto name = bison::lookup_registered_key_name(h))
    return *name;
  std::ostringstream s;
  s << '#' << std::hex << std::setw(8) << std::setfill('0') << h;
  return s.str();
}

} // namespace

void server::on_request_trace(context& ctx, const shared::envelope& env) {
  bison::print_options popts;
  popts.multiline = false;
  popts.hide_internal = true;

  std::ostringstream oss;
  oss << "[rmi] " << op_to_label(env.op) << " sid=0x" << std::hex << std::setw(8) << std::setfill('0')
      << ctx.session_id.id;

  const bison::key_t op = env.op;
  if (op == OP_INSTANTIATE || op == OP_DESCRIBE) {
    const auto* f = env.payload.findField(FIELD_KLASS);
    if (f && f->is<bison::key_t>())
      oss << " class=" << resolve(f->as<bison::key_t>().id);
  } else if (op == OP_CALL) {
    oss << std::dec << " obj=0x" << std::hex << std::setw(8) << std::setfill('0') << env.object_id.id;
    const auto* f = env.payload.findField(FIELD_NAME);
    if (f && f->is<bison::key_t>())
      oss << " method=" << resolve(f->as<bison::key_t>().id);
    const auto* pf = env.payload.findField(FIELD_PARAMS);
    if (pf && pf->is<bison::dynamic_ptr>()) {
      auto ptr = pf->as<bison::dynamic_ptr>();
      if (ptr && !ptr->empty())
        oss << " args=" << bison::print(*ptr, popts);
    }
  } else if (op == OP_SET) {
    oss << std::dec << " obj=0x" << std::hex << std::setw(8) << std::setfill('0') << env.object_id.id;
    if (!env.payload.empty())
      oss << " " << bison::print(env.payload, popts);
  } else if (op == OP_GET || op == OP_DESTROY || op == OP_CLEAR) {
    oss << std::dec << " obj=0x" << std::hex << std::setw(8) << std::setfill('0') << env.object_id.id;
  }

  on_print(ctx.session_id, oss.str());
}

void server::on_response_trace(
    context& ctx,
    const shared::envelope& request_env,
    bison::key_t op,
    bool is_error,
    bison::key_t error_code,
    const bison::dynamic& response_payload) {
  bison::print_options popts;
  popts.multiline = false;
  popts.hide_internal = true;

  std::ostringstream oss;
  oss << "[rmi] " << op_to_label(op) << (is_error ? " ERROR" : " ok   ") << " sid=0x" << std::hex << std::setw(8)
      << std::setfill('0') << ctx.session_id.id;

  if (is_error) {
    oss << " code=0x" << error_code.id;
  } else if (!response_payload.empty()) {
    if (op == OP_INSTANTIATE) {
      const auto* kf = response_payload.findField(FIELD_KLASS);
      if (kf && kf->is<bison::key_t>())
        oss << " class=" << resolve(kf->as<bison::key_t>().id);
      const auto* of = response_payload.findField(FIELD_OBJECT_ID);
      if (of && of->is<bison::key_t>())
        oss << std::dec << " obj=0x" << std::hex << std::setw(8) << std::setfill('0') << of->as<bison::key_t>().id;
    } else if (op == OP_CALL || op == OP_GET) {
      oss << std::dec << " obj=0x" << std::hex << std::setw(8) << std::setfill('0') << request_env.object_id.id;
      if (!response_payload.empty())
        oss << " " << bison::print(response_payload, popts);
    }
  }

  on_print(ctx.session_id, oss.str());
}

// ── Internal helpers ──────────────────────────────────────────────────────────

namespace {

/**
 * @brief Read a key token from a field accepting either `key_t` or `hash_t`.
 * @param obj Source object.
 * @param field_name Field key to read.
 * @return Parsed key token.
 * @throws std::runtime_error if the field is missing or incompatible.
 */
bison::key_t read_key_token(const bison::dynamic& obj, bison::key_t field_name) {
  const auto* f = obj.findField(field_name);
  if (f == nullptr) {
    throw std::runtime_error("Missing key token field");
  }
  if (f->is<bison::key_t>()) {
    return f->as<bison::key_t>();
  }
  if (f->is<bison::hash_t>()) {
    return bison::key_t{f->as<bison::hash_t>()};
  }
  throw std::runtime_error("Invalid key token type");
}

/**
 * @brief Apply `DisplayName`, `Description`, `Category`, `Obsolete`, and
 *        `Required` attributes from @p f to @p desc.
 * @return `true` if at least one attribute was written to @p desc.
 */
static bool apply_attrs(bison::dynamic& desc, const bison::field& f) {
  using namespace bison;
  bool applied = false;
  if (auto* dn = f.findAttribute<DisplayName>()) {
    desc[FIELD_DISPLAY_NAME] = dn->name();
    applied = true;
  }
  if (auto* d = f.findAttribute<Description>()) {
    desc[FIELD_DESCRIPTION] = d->text();
    applied = true;
  }
  if (auto* c = f.findAttribute<Category>()) {
    desc[FIELD_CATEGORY] = c->name();
    applied = true;
  }
  if (auto* o = f.findAttribute<Obsolete>()) {
    desc[FIELD_OBSOLETE] = bool{true};
    if (!o->message().empty())
      desc[FIELD_OBSOLETE_MESSAGE] = o->message();
    applied = true;
  }
  if (f.findAttribute<Required>()) {
    desc[FIELD_REQUIRED] = bool{true};
    applied = true;
  }
  return applied;
}

/** @brief Apply attributes from a method_entry to @p desc.
 *  @return `true` if at least one attribute was written. */
static bool apply_method_attrs(bison::dynamic& desc, const bison::method& e) {
  using namespace bison;
  bool applied = false;
  if (auto* dn = e.findAttribute<DisplayName>()) {
    desc[FIELD_DISPLAY_NAME] = dn->name();
    applied = true;
  }
  if (auto* d = e.findAttribute<Description>()) {
    desc[FIELD_DESCRIPTION] = d->text();
    applied = true;
  }
  if (auto* c = e.findAttribute<Category>()) {
    desc[FIELD_CATEGORY] = c->name();
    applied = true;
  }
  if (auto* o = e.findAttribute<Obsolete>()) {
    desc[FIELD_OBSOLETE] = bool{true};
    if (!o->message().empty())
      desc[FIELD_OBSOLETE_MESSAGE] = o->message();
    applied = true;
  }
  if (e.findAttribute<Required>())
    desc[FIELD_REQUIRED] = bool{true};
  return applied;
}

} // namespace

// ── Lifecycle
// ─────────────────────────────────────────────────────────────────

/** @brief Stops background threads if still running. */
server::~server() {
  if (running_.load()) {
    try {
      stop();
    } catch (...) {
    }
  }
}

/** @copydoc bdg::bison::rmi::server::listen */
void server::listen(bison::dynamic params, auth_module_ptr auth_module) {
  shared::register_all_schemas();
  auth_module_ = std::move(auth_module);
  running_.store(true);
  transport_.get()->start(std::move(params));

  const size_t worker_count = std::max<size_t>(1, std::thread::hardware_concurrency());
  dispatch_workers_.reserve(worker_count);
  for (size_t i = 0; i < worker_count; ++i)
    dispatch_workers_.push_back(std::make_unique<dispatch_worker_state>());
  for (size_t i = 0; i < worker_count; ++i)
    dispatch_workers_[i]->thread = std::thread(&server::dispatch_worker, this, i);

  accept_thread_ = std::thread(&server::accept_loop, this);
}

/** @copydoc bdg::bison::rmi::server::stop */
void server::stop() {
  running_.store(false);
  transport_.get()->stop();
  if (accept_thread_.joinable())
    accept_thread_.join();
  join_workers();
}

/** @copydoc bdg::bison::rmi::server::enable_profiling */
void server::enable_profiling(std::filesystem::path output_dir) {
  profiler_service_ = register_profiler_class(std::move(output_dir));
}

/**
 * @brief Join the bounded dispatch worker pool.
 *
 * Each worker tears down any sessions still assigned to it (see
 * `dispatch_worker()`'s shutdown sweep) as the last thing it does before
 * its thread function returns, so by the time every thread here has been
 * joined, every session has been torn down exactly once.
 */
void server::join_workers() {
  for (auto& w : dispatch_workers_)
    if (w->thread.joinable())
      w->thread.join();
  dispatch_workers_.clear();
}

// ── Accept loop
// ───────────────────────────────────────────────────────────────

/**
 * @brief Accept incoming connections and hand each to a dispatch worker.
 */
void server::accept_loop() {
  size_t next_worker = 0;
  while (running_.load(std::memory_order_acquire)) {
    auto conn = transport_.get()->accept(std::chrono::milliseconds{100});
    if (!conn)
      continue;
    start_session(std::move(conn), next_worker);
    next_worker = (next_worker + 1) % dispatch_workers_.size();
  }
}

/**
 * @brief Create a session for a freshly-accepted connection and assign it
 *        to dispatch worker @p worker_index.
 */
void server::start_session(std::unique_ptr<transport::server_connection_iface> conn, size_t worker_index) {
  auto slot = std::make_shared<session_slot>();
  slot->session_id = shared::generate_id();
  slot->ctx_holder = std::make_shared<bison::synchronized<std::unique_ptr<context>>>(
      std::in_place, on_create_context(slot->session_id));

  // Safe to capture the raw connection pointer (not conn, which is about to
  // be moved into slot): slot and its conn share one lifetime from here on,
  // and emit_event never outlives ctx_holder, which lives inside slot.
  auto* raw_conn = conn.get();
  {
    auto lp = slot->ctx_holder->wlock();
    (*lp)->emit_event = [raw_conn](bison::key_t oid, bison::key_t name, bison::dynamic params) {
      bison::dynamic ev_payload;
      ev_payload[FIELD_NAME] = name;
      ev_payload[FIELD_PARAMS] = bison::dynamic_ptr{std::move(params)};
      shared::envelope event_env;
      event_env.kind = KIND_EVENT;
      event_env.op = OP_EVENT;
      event_env.object_id = oid;
      event_env.oneway = true;
      event_env.payload = std::move(ev_payload);
      auto frame = event_env.encode();
      raw_conn->send(std::move(frame));
    };
  }
  slot->conn = std::move(conn);

  // Register the context so external observers can access it.
  session_contexts_.wlock()->emplace(slot->session_id.id, slot->ctx_holder);
  {
    auto lp = slot->ctx_holder->wlock();
    on_session_created(**lp);
  }

  dispatch_workers_[worker_index]->sessions.withWLock([&](auto& v) { v.push_back(std::move(slot)); });
}

// ── Dispatch worker
// ───────────────────────────────────────────────────────

/**
 * @brief Round-robin over this worker's assigned sessions until shutdown.
 * @param worker_index Index into `dispatch_workers_` for this thread.
 */
void server::dispatch_worker(size_t worker_index) {
  auto& w = *dispatch_workers_[worker_index];

  while (running_.load(std::memory_order_acquire)) {
    // Snapshot the current session list rather than holding the lock across
    // the receive() calls below (each up to kDispatchPollTimeout) -- doing
    // so would block start_session() (which needs the write lock to assign
    // new connections to this worker) for the whole lap.
    std::vector<std::shared_ptr<session_slot>> current;
    w.sessions.withRLock([&](auto& v) { current = v; });

    if (current.empty()) {
      std::this_thread::sleep_for(kDispatchPollTimeout);
      continue;
    }

    std::vector<std::shared_ptr<session_slot>> closed;
    for (auto& slot : current) {
      if (!running_.load(std::memory_order_acquire))
        break;
      if (slot->conn->is_closed()) {
        closed.push_back(slot);
        continue;
      }
      service_session(*slot);
      if (slot->conn->is_closed())
        closed.push_back(slot);
    }

    if (!closed.empty()) {
      for (auto& slot : closed)
        teardown_session(*slot);
      w.sessions.withWLock([&](auto& v) {
        v.erase(std::remove_if(
                    v.begin(), v.end(),
                    [&](const auto& s) { return std::find(closed.begin(), closed.end(), s) != closed.end(); }),
                v.end());
      });
    }
  }

  // Shutdown: tear down every session still assigned to this worker.
  std::vector<std::shared_ptr<session_slot>> remaining;
  w.sessions.withWLock([&](auto& v) {
    remaining = std::move(v);
    v.clear();
  });
  for (auto& slot : remaining)
    teardown_session(*slot);
}

/**
 * @brief Try to receive and dispatch one frame for @p slot; a no-op if
 *        nothing arrived within `kDispatchPollTimeout`.
 */
void server::service_session(session_slot& slot) {
  bison::buffer frame;
  if (!slot.conn->receive(frame, kDispatchPollTimeout))
    return;

  shared::envelope env;
  try {
    env = shared::envelope::decode(frame);
  } catch (const std::exception& e) {
    try {
      bison::dynamic error_payload{CLASS_ERROR};
      error_payload[FIELD_ERROR_CODE] = ERR_INVALID_REQUEST;
      error_payload[FIELD_ERROR_MESSAGE] = std::string{e.what()};

      shared::envelope frame_err_env;
      frame_err_env.kind = KIND_RESPONSE;
      frame_err_env.op = OP_CONNECT;
      frame_err_env.oneway = false;
      frame_err_env.error = std::move(error_payload);
      auto frame_err = frame_err_env.encode();
      slot.conn->send(std::move(frame_err));
    } catch (...) {
    }
    return;
  }

  auto lp = slot.ctx_holder->wlock();
  context& ctx = **lp;
  on_before_dispatch(ctx);
  try {
    handle_request(ctx, env, *slot.conn);
    on_after_dispatch(ctx);
  } catch (const std::exception& e) {
    on_after_dispatch(ctx);
    try {
      send_error(ctx, *slot.conn, env, env.op, ERR_INVALID_REQUEST, e.what());
    } catch (...) {
    }
  }
}

/**
 * @brief Run session teardown hooks and unregister @p slot.
 *
 * Called exactly once per session, either when its connection closes or
 * during a dispatch worker's shutdown sweep.
 */
void server::teardown_session(session_slot& slot) {
  {
    auto lp = slot.ctx_holder->wlock();
    context& ctx = **lp;
    // Bracket teardown in the same on_before_dispatch/on_after_dispatch
    // hooks used around handle_request() in service_session() -- overrides
    // rely on these to know a thread already holds ctx's wlock (e.g. so a
    // destructor invoked from cleanup_context()'s HOOK_DESTRUCT calls /
    // ctx.objects.clear() doesn't try to re-acquire it and self-deadlock).
    // Without this, on_session_destroyed() was covered (called directly
    // under the wlock) but cleanup_context() -- where objects, and
    // therefore their destructors, actually run -- was not.
    on_before_dispatch(ctx);
    on_session_destroyed(ctx);
    cleanup_context(ctx);
    on_after_dispatch(ctx);
  }

  // Unregister and clear emit_event to prevent dangling references to conn.
  session_contexts_.wlock()->erase(slot.session_id.id);
}

// ── Envelope helpers
// ──────────────────────────────────────────────────────────

/**
 * @brief Send a protocol response envelope.
 */
void server::send_response(
    context& ctx,
    transport::server_connection_iface& conn,
    const shared::envelope& env,
    bison::key_t op,
    bison::dynamic payload) {
  shared::envelope response_env;
  response_env.kind = KIND_RESPONSE;
  response_env.op = op;
  response_env.request_id = env.request_id;
  response_env.object_id = env.object_id;
  response_env.oneway = false;
  response_env.payload = std::move(payload);
  conn.send(response_env.encode());
  on_response_trace(ctx, env, op, false, bison::key_t{0u}, response_env.payload);
}

/**
 * @brief Send an error response envelope.
 */
void server::send_error(
    context& ctx,
    transport::server_connection_iface& conn,
    const shared::envelope& env,
    bison::key_t op,
    bison::key_t code,
    const std::string& message) {
  bison::dynamic error_payload{CLASS_ERROR};
  error_payload[FIELD_ERROR_CODE] = code;
  error_payload[FIELD_ERROR_MESSAGE] = message;

  shared::envelope response_env;
  response_env.kind = KIND_RESPONSE;
  response_env.op = op;
  response_env.request_id = env.request_id;
  response_env.object_id = env.object_id;
  response_env.oneway = false;
  response_env.error = std::move(error_payload);
  conn.send(response_env.encode());
  on_response_trace(ctx, env, op, true, code, bison::dynamic{});
}

// ── Request dispatch
// ──────────────────────────────────────────────────────────

/**
 * @brief Validate and route one request envelope to its operation handler.
 */
void server::handle_request(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn) {
  if (env.op == 0u) {
    send_error(ctx, conn, env, OP_CONNECT, ERR_INVALID_REQUEST, "Missing operation token");
    return;
  }

  if (env.kind != KIND_REQUEST) {
    send_error(ctx, conn, env, OP_CONNECT, ERR_INVALID_REQUEST, "Unexpected non-request envelope");
    return;
  }

  int32_t version = env.version;
  if (version != PROTOCOL_VERSION) {
    send_error(ctx, conn, env, OP_CONNECT, ERR_UNSUPPORTED_VERSION, "Unsupported protocol version");
    return;
  }

  if (try_handle_request(ctx, env, conn))
    return;

  // Set for the duration of this request's dispatch so any object created
  // while handling it -- directly via handle_instantiate, or indirectly by
  // application code that creates objects as a side effect of another op
  // (e.g. wish's UI template system, inside handle_call) -- is filed under
  // the same group, as long as it goes through context::put_object(). 0
  // (no group set on the envelope) is the default for ordinary clients, so
  // this is a no-op unless something (e.g. rmi::bridge) explicitly tags the
  // request.
  ctx.current_group = env.group;

  on_request_trace(ctx, env);

  using handler_fn = void (server::*)(context&, const shared::envelope&, transport::server_connection_iface&);
  static const std::unordered_map<bison::key_t, handler_fn, bison::key_t, bison::key_t> handler_map = {
      {OP_CONNECT, &server::handle_connect},
      {OP_DESCRIBE, &server::handle_describe},
      {OP_INSTANTIATE, &server::handle_instantiate},
      {OP_CLEAR, &server::handle_clear},
      {OP_SET, &server::handle_set},
      {OP_GET, &server::handle_get},
      {OP_CALL, &server::handle_call},
      {OP_DESTROY, &server::handle_destroy},
      {OP_DESTROY_GROUP, &server::handle_destroy_group},
      {OP_DISCONNECT, &server::handle_disconnect},
      {OP_DICTIONARY, &server::handle_dictionary},
      {OP_HELP, &server::handle_help},
  };

  bison::key_t op = env.op;
  auto it = handler_map.find(op);
  if (it != handler_map.end())
    (this->*it->second)(ctx, env, conn);
  else
    send_error(ctx, conn, env, op, ERR_UNKNOWN_OPERATION, "Unknown operation");
}

// ── Operation handlers
// ────────────────────────────────────────────────────────

/** @brief Handle `connect` handshake requests. */
void server::handle_connect(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn) {
  if (auth_module_) {
    std::string identity;
    if (!auth_module_->authenticate(ctx, env.payload, identity)) {
      send_error(ctx, conn, env, OP_CONNECT, ERR_ACCESS_DENIED, "Authentication failed");
      return;
    }
    on_authenticated(ctx, identity);
  }

  bison::dynamic resp;
  resp[FIELD_VERSION] = int32_t{PROTOCOL_VERSION};
  send_response(ctx, conn, env, OP_CONNECT, std::move(resp));
}

/** @brief Handle class metadata requests. */
void server::handle_describe(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn) {
  const auto& p = env.payload;

  bison::key_t requested = p.as<bison::key_t>(FIELD_KLASS);
  bison::dynamic resp;

  {
    auto lp = bison::dynamic::getRegistry().rlock();
    const auto& nsmap = *lp;

    if (static_cast<bison::hash_t>(requested) == 0u) {
      // List all classes across all namespaces.
      size_t idx = 0;
      for (const auto& [ns, classes] : nsmap) {
        for (const auto& [klass, proto] : classes) {
          if (klass == CLASS_ENVELOPE)
            continue;
          bison::dynamic desc;
          desc[FIELD_KLASS] = klass;
          // Attach class-level attribute metadata from the CLASS field.
          const auto* class_field = proto->findField(bison::dynamic::CLASS);
          if (class_field)
            apply_attrs(desc, *class_field);
          resp[idx++] = bison::dynamic_ptr{std::move(desc)};
        }
      }
    } else {
      // Find a specific class by searching all namespaces.
      const bison::dynamic* proto = nullptr;
      for (const auto& [ns, classes] : nsmap) {
        auto it = classes.find(requested);
        if (it != classes.end()) {
          proto = it->second.get();
          break;
        }
      }
      if (!proto) {
        lp.unlock();
        send_error(ctx, conn, env, OP_DESCRIBE, ERR_CLASS_NOT_FOUND, "Class not found");
        return;
      }
      resp[FIELD_KLASS] = requested;
      // Attach class-level attribute metadata from the CLASS field.
      const auto* class_field = proto->findField(bison::dynamic::CLASS);
      if (class_field)
        apply_attrs(resp, *class_field);
      // Copy prototype field values and collect per-field metadata.
      bison::dynamic fields_meta;
      bool has_field_meta = false;
      proto->forEach([&](bison::key_t k, const bison::field& v) {
        resp[k] = v;
        // Skip reserved internal fields in the per-field metadata map.
        if (k == bison::dynamic::CLASS || k == bison::dynamic::PARENT || k == bison::dynamic::NAMESPACE)
          return;
        bison::dynamic fmeta;
        if (apply_attrs(fmeta, v)) {
          fields_meta[k] = bison::dynamic_ptr{std::move(fmeta)};
          has_field_meta = true;
        }
      });
      if (has_field_meta)
        resp[FIELD_FIELDS] = bison::dynamic_ptr{std::move(fields_meta)};
      // Collect per-method metadata (always emitted when methods are present).
      bison::dynamic methods_meta;
      bool has_methods = false;
      proto->forEachMethod([&](bison::key_t k, const bison::method& e) {
        bison::dynamic mmeta;
        apply_method_attrs(mmeta, e);
        methods_meta[k] = bison::dynamic_ptr{std::move(mmeta)};
        has_methods = true;
      });
      if (has_methods)
        resp[FIELD_METHODS] = bison::dynamic_ptr{std::move(methods_meta)};
    }
  }

  send_response(ctx, conn, env, OP_DESCRIBE, std::move(resp));
}

/** @brief Handle server-side object instantiation requests. */
void server::handle_instantiate(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn) {
  const auto& p = env.payload;

  bison::key_t klass = p.as<bison::key_t>(FIELD_KLASS);
  bison::key_t ns{0U};

  if (p.findField(FIELD_NAMESPACE) != nullptr) {
    try {
      ns = read_key_token(p, FIELD_NAMESPACE);
    } catch (const std::exception& e) {
      send_error(ctx, conn, env, OP_INSTANTIATE, ERR_INVALID_REQUEST, e.what());
      return;
    }
  }

  // Allow subclasses (e.g. rmi::bridge) to bypass the registry check.
  if (!on_check_class(ctx, ns, klass)) {
    send_error(ctx, conn, env, OP_INSTANTIATE, ERR_CLASS_NOT_FOUND, "Class not registered in requested namespace");
    return;
  }

  bison::dynamic_ptr obj;
  try {
    obj = on_create_object(ctx, ns, klass);
  } catch (const std::exception& e) {
    send_error(ctx, conn, env, OP_INSTANTIATE, ERR_INTERNAL_ERROR, e.what());
    return;
  }

  if (!obj) {
    send_error(ctx, conn, env, OP_INSTANTIATE, ERR_INTERNAL_ERROR, "on_create_object returned null");
    return;
  }

  if (obj->findMethod(HOOK_CONSTRUCT) != nullptr) {
    try {
      bison::dynamic construct_params;
      auto& pf = p[FIELD_PARAMS];
      if (pf.is<bison::dynamic_ptr>()) {
        auto ptr = pf.as<bison::dynamic_ptr>();
        if (ptr)
          construct_params = std::move(*ptr);
      }
      obj->call(HOOK_CONSTRUCT, construct_params);
    } catch (const std::exception& e) {
      send_error(ctx, conn, env, OP_INSTANTIATE, ERR_INTERNAL_ERROR, std::string("__construct failed: ") + e.what());
      return;
    }
  }

  const bison::key_t oid = shared::generate_id();
  ctx.put_object(oid, obj);

  bison::dynamic resp;
  resp[FIELD_OBJECT_ID] = oid;
  resp[FIELD_KLASS] = klass;
  send_response(ctx, conn, env, OP_INSTANTIATE, std::move(resp));
}

/** @brief Handle clear requests for a live remote object. */
void server::handle_clear(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn) {
  bison::key_t oid = env.object_id;
  auto it = ctx.objects.find(oid.id);
  if (it == ctx.objects.end()) {
    send_error(ctx, conn, env, OP_CLEAR, ERR_OBJECT_NOT_FOUND, "Object not found");
    return;
  }

  bison::key_t klass_key = it->second->as<bison::key_t>(bison::dynamic::CLASS);
  {
    // Resolve the namespace from the existing object so we look in the
    // correct collection.
    bison::key_t ns{0U};
    auto* nsField = it->second->findField(bison::dynamic::NAMESPACE);
    if (nsField && nsField->is<bison::key_t>()) {
      ns = nsField->as<bison::key_t>();
    }
    auto lp = bison::dynamic::getRegistry().rlock();
    const auto& nsmap = *lp;
    auto nsIt = nsmap.find(ns);
    if (nsIt != nsmap.end()) {
      auto class_it = nsIt->second.find(klass_key);
      if (class_it != nsIt->second.end() && class_it->second) {
        *it->second = class_it->second->clone();
      } else {
        *it->second = bison::dynamic::instantiate(ns, klass_key);
      }
    } else {
      *it->second = bison::dynamic::instantiate(klass_key);
    }
  }

  if (it->second->findMethod(HOOK_CLEAR) != nullptr) {
    try {
      it->second->call(HOOK_CLEAR, bison::dynamic{});
    } catch (...) {
    }
  }

  send_response(ctx, conn, env, OP_CLEAR, bison::dynamic{});
}

/** @brief Handle partial field updates for a live remote object. */
void server::handle_set(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn) {
  bison::key_t oid = env.object_id;
  auto it = ctx.objects.find(oid.id);
  if (it == ctx.objects.end()) {
    send_error(ctx, conn, env, OP_SET, ERR_OBJECT_NOT_FOUND, "Object not found");
    return;
  }
  auto& obj = *it->second;

  // NAMESPACE must never be copied from a patch -- see the identical
  // exclusion in standalone::handle_set() for the full explanation: a
  // __setter hook probing patch.findField<T>() for an optional field can
  // miss and cause resolveNamespace() to cache a spurious __namespace=0
  // onto `patch`, which would otherwise overwrite the target object's
  // real namespace here and break its method resolution.
  auto apply_patch = [&obj](const bison::dynamic& patch) {
    patch.forEach([&obj](bison::key_t k, const bison::field& v) {
      if (k != bison::dynamic::CLASS && k != bison::dynamic::PARENT && k != bison::dynamic::NAMESPACE)
        obj[k] = v;
    });
  };

  // Only clone the incoming payload when a __setter hook actually needs an
  // owned/mutable copy to transform; the common case (no hook) applies
  // env.payload's fields directly with no copy at all.
  if (obj.findMethod(HOOK_SETTER) != nullptr) {
    bison::dynamic patched;
    try {
      patched = obj.call(HOOK_SETTER, env.payload);
    } catch (const std::exception& e) {
      send_error(ctx, conn, env, OP_SET, ERR_INTERNAL_ERROR, std::string("__setter failed: ") + e.what());
      return;
    }
    apply_patch(patched);
  } else {
    apply_patch(env.payload);
  }

  send_response(ctx, conn, env, OP_SET, bison::dynamic{});
}

/** @brief Handle object reads with optional projection payload. */
void server::handle_get(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn) {
  bison::key_t oid = env.object_id;
  auto it = ctx.objects.find(oid.id);
  if (it == ctx.objects.end()) {
    send_error(ctx, conn, env, OP_GET, ERR_OBJECT_NOT_FOUND, "Object not found");
    return;
  }
  auto& obj = *it->second;

  const auto& projection = env.payload;

  bool has_projection = false;
  projection.forEach([&](bison::key_t k, const bison::field&) {
    if (k != bison::dynamic::CLASS && k != bison::dynamic::PARENT)
      has_projection = true;
  });

  bison::dynamic result;
  if (!has_projection) {
    // Copy-construct directly rather than obj.clone(): clone() goes through
    // the virtual clone_ptr(), which heap-allocates a whole shared_ptr
    // control block just to immediately unwrap and discard it. The plain
    // copy constructor performs the identical deep copy (including cloning
    // nested dynamic_ptr fields) without that extra allocation.
    result = bison::dynamic{obj};
  } else {
    projection.forEach([&](bison::key_t k, const bison::field&) {
      if (k == bison::dynamic::CLASS || k == bison::dynamic::PARENT)
        return;
      auto* f = obj.findField(k);
      if (f)
        result[k] = *f;
    });
  }

  if (obj.findMethod(HOOK_GETTER) != nullptr) {
    try {
      result = obj.call(HOOK_GETTER, result);
    } catch (...) {
    }
  }

  send_response(ctx, conn, env, OP_GET, std::move(result));
}

/** @brief Handle method invocation requests. */
void server::handle_call(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn) {
  bison::key_t oid = env.object_id;
  auto it = ctx.objects.find(oid.id);
  if (it == ctx.objects.end()) {
    send_error(ctx, conn, env, OP_CALL, ERR_OBJECT_NOT_FOUND, "Object not found");
    return;
  }
  auto& obj = *it->second;

  const auto& params = env.payload;

  bison::key_t method_name = read_key_token(params, FIELD_NAME);
  bool oneway = env.oneway;

  bison::dynamic args;
  auto& params_field = params[FIELD_PARAMS];
  if (params_field.is<bison::dynamic_ptr>()) {
    auto ptr = params_field.as<bison::dynamic_ptr>();
    if (ptr)
      args = std::move(*ptr);
  }

  try {
    bison::dynamic res = obj.call(method_name, args);
    if (!oneway)
      send_response(ctx, conn, env, OP_CALL, std::move(res));
  } catch (const std::exception& e) {
    if (!oneway)
      send_error(ctx, conn, env, OP_CALL, ERR_INTERNAL_ERROR, e.what());
  }
}

/** @brief Handle explicit object destruction requests. */
void server::handle_destroy(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn) {
  bison::key_t oid = env.object_id;
  auto it = ctx.objects.find(oid.id);
  if (it == ctx.objects.end()) {
    send_error(ctx, conn, env, OP_DESTROY, ERR_OBJECT_NOT_FOUND, "Object not found");
    return;
  }

  if (it->second->findMethod(HOOK_DESTRUCT) != nullptr) {
    try {
      it->second->call(HOOK_DESTRUCT, bison::dynamic{});
    } catch (...) {
    }
  }

  ctx.objects.erase(it);
  send_response(ctx, conn, env, OP_DESTROY, bison::dynamic{});
}

/** @brief Handle group destruction requests (see context::groups). */
void server::handle_destroy_group(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn) {
  const bison::hash_t group = static_cast<bison::hash_t>(env.group);

  auto git = ctx.groups.find(group);
  if (git == ctx.groups.end() || group == 0u) {
    // Unknown/empty/unset group: nothing to destroy, not an error --
    // matches destroying an already-empty collection being a no-op.
    send_response(ctx, conn, env, OP_DESTROY_GROUP, bison::dynamic{});
    return;
  }

  // Move the id set out and erase the group entry before running any
  // __destruct hooks, so a hook that (unusually) tries to create another
  // object in the same group doesn't get folded into the set we're
  // currently iterating.
  auto ids = std::move(git->second);
  ctx.groups.erase(git);

  for (bison::hash_t id : ids) {
    auto it = ctx.objects.find(id);
    if (it == ctx.objects.end())
      continue;
    if (it->second && it->second->findMethod(HOOK_DESTRUCT) != nullptr) {
      try {
        it->second->call(HOOK_DESTRUCT, bison::dynamic{});
      } catch (...) {
      }
    }
    ctx.objects.erase(it);
  }

  send_response(ctx, conn, env, OP_DESTROY_GROUP, bison::dynamic{});
}

/** @brief Handle disconnect requests and close the connection context. */
void server::handle_disconnect(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn) {
  cleanup_context(ctx);
  // Acknowledge before closing: client::disconnect() blocks on this response
  // (see its doc comment) so it never tears down its own read side -- which,
  // on term_transport, is what strips incoming BISON<...> markers from the
  // wire -- until the server has genuinely finished cleanup_context() and is
  // no longer going to push anything further for this session.
  try {
    send_response(ctx, conn, env, OP_DISCONNECT, bison::dynamic{});
  } catch (...) {
  }
  conn.close();
}

/** @brief Handle hash→display-name dictionary requests. */
void server::handle_dictionary(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn) {
  using namespace bison;
  dynamic dict;
  {
    auto lp = dynamic::getRegistry().rlock();
    for (const auto& [ns_key, classes] : *lp) {
      for (const auto& [klass_key, proto] : classes) {
        if (klass_key == CLASS_ENVELOPE)
          continue;
        // Class-level DisplayName
        const auto* class_field = proto->findField(dynamic::CLASS);
        if (class_field) {
          if (auto* dn = class_field->findAttribute<DisplayName>())
            dict[klass_key] = dn->name();
        }
        // Field DisplayNames
        proto->forEach([&](key_t k, const field& f) {
          if (k == dynamic::CLASS || k == dynamic::PARENT || k == dynamic::NAMESPACE)
            return;
          if (auto* dn = f.findAttribute<DisplayName>())
            dict[k] = dn->name();
        });
        // Method DisplayNames and parameter field DisplayNames
        proto->forEachMethod([&](key_t k, const method& m) {
          if (auto* dn = m.findAttribute<DisplayName>())
            dict[k] = dn->name();
          auto add_param_fields = [&](const dynamic* d) {
            if (!d)
              return;
            d->forEach([&](key_t fk, const field& f) {
              if (auto* dn = f.findAttribute<DisplayName>())
                dict[fk] = dn->name();
            });
          };
          add_param_fields(m.inputSpec());
          add_param_fields(m.outputSpec());
        });
      }
    }
  }
  send_response(ctx, conn, env, OP_DICTIONARY, std::move(dict));
}

/** @brief Handle human-readable help text requests. */
void server::handle_help(context& ctx, const shared::envelope& env, transport::server_connection_iface& conn) {
  using namespace bison;
  std::string preamble = on_help_text();
  std::ostringstream oss;
  if (!preamble.empty())
    oss << preamble << "\n\n";
  oss << "Registered classes:\n";

  {
    auto lp = dynamic::getRegistry().rlock();
    for (const auto& [ns_key, classes] : *lp) {
      for (const auto& [klass_key, proto] : classes) {
        if (klass_key == CLASS_ENVELOPE)
          continue;
        // Only show classes that have a DisplayName.
        const auto* class_field = proto->findField(dynamic::CLASS);
        if (!class_field)
          continue;
        auto* cdn = class_field->findAttribute<DisplayName>();
        if (!cdn)
          continue;
        oss << "  " << cdn->name();
        if (auto* cd = class_field->findAttribute<Description>())
          oss << " -- " << cd->text();
        oss << '\n';
        // Fields with DisplayName
        bool first_field = true;
        proto->forEach([&](key_t k, const field& f) {
          if (k == dynamic::CLASS || k == dynamic::PARENT || k == dynamic::NAMESPACE)
            return;
          auto* dn = f.findAttribute<DisplayName>();
          if (!dn)
            return;
          if (first_field) {
            oss << "    Fields:\n";
            first_field = false;
          }
          oss << "      " << dn->name();
          if (auto* d = f.findAttribute<Description>())
            oss << " -- " << d->text();
          oss << '\n';
        });
        // Methods with DisplayName
        bool first_method = true;
        proto->forEachMethod([&](key_t k, const method& m) {
          (void)k;
          auto* dn = m.findAttribute<DisplayName>();
          if (!dn)
            return;
          if (first_method) {
            oss << "    Methods:\n";
            first_method = false;
          }
          oss << "      " << dn->name();
          if (auto* d = m.findAttribute<Description>())
            oss << " -- " << d->text();
          oss << '\n';
        });
      }
    }
  }

  dynamic resp;
  resp[FIELD_DESCRIPTION] = oss.str();
  send_response(ctx, conn, env, OP_HELP, std::move(resp));
}

/**
 * @brief Run destruction hooks for all objects and clear the session context.
 */
void server::cleanup_context(context& ctx) {
  for (auto& [oid, obj] : ctx.objects) {
    if (obj && obj->findMethod(HOOK_DESTRUCT) != nullptr) {
      try {
        obj->call(HOOK_DESTRUCT, bison::dynamic{});
      } catch (...) {
      }
    }
  }
  ctx.objects.clear();
  ctx.groups.clear();
  ctx.emit_event = nullptr;
}

} // namespace bdg::bison::rmi
