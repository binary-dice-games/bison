// MIT License © 2025 Binary Dice Games
/**
 * @file server.cpp
 * @brief Implementation of the RMI server accept loop and request handlers.
 */
#include "src/rmi/server/server.hpp"

#include "src/rmi/shared/constants.hpp"
#include "src/rmi/shared/ids.hpp"
#include "src/rmi/shared/schemas.hpp"

#include <shared_mutex>
#include <stdexcept>

namespace bdg::bison::rmi {

using namespace shared::constants;
using namespace transport;

namespace {

/**
 * @brief Read a key token from a field accepting either `key_t` or `hash_t`.
 * @param obj Source object.
 * @param field_name Field key to read.
 * @return Parsed key token.
 * @throws std::runtime_error if the field is missing or incompatible.
 */
bison::key_t read_key_token(
    const bison::dynamic& obj,
    bison::key_t field_name) {
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
void server::listen(bison::dynamic params) {
  shared::register_all_schemas();
  running_.store(true);
  transport_raw_->start(std::move(params));
  accept_thread_ = std::thread(&server::accept_loop, this);
}

/** @copydoc bdg::bison::rmi::server::stop */
void server::stop() {
  running_.store(false);
  transport_raw_->stop();
  if (accept_thread_.joinable())
    accept_thread_.join();
  join_workers();
}

/**
 * @brief Join and clear all per-connection worker threads.
 */
void server::join_workers() {
  auto lp = workers_.wlock();
  for (auto& t : *lp)
    if (t.joinable())
      t.join();
  lp->clear();
}

// ── Accept loop
// ───────────────────────────────────────────────────────────────

/**
 * @brief Accept incoming connections and spawn per-client workers.
 */
void server::accept_loop() {
  while (running_.load(std::memory_order_acquire)) {
    auto conn = transport_raw_->accept(std::chrono::milliseconds{100});
    if (!conn)
      continue;
    workers_.wlock()->emplace_back(
        std::thread(&server::client_worker, this, std::move(conn)));
  }
}

// ── Client worker
// ─────────────────────────────────────────────────────────────

/**
 * @brief Process one client connection until closed.
 * @param conn Active connection object.
 */
void server::client_worker(std::unique_ptr<transport::server_connection_iface> conn) {
  auto ctx_ptr = std::make_shared<context>();
  context& ctx = *ctx_ptr;
  ctx.session_id = shared::generate_id();
  ctx.emit_event =
      [&conn](bison::key_t oid, bison::key_t name, bison::dynamic params) {
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
        conn->send(std::move(frame));
      };

  // Register the context so external observers can access it.
  session_contexts_.wlock()->emplace(ctx.session_id.id, ctx_ptr);

  while (!conn->is_closed()) {
    bison::buffer frame;
    if (!conn->receive(frame, std::chrono::milliseconds{50}))
      continue;

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
        conn->send(std::move(frame_err));
      } catch (...) {
      }
      continue;
    }

    try {
      handle_request(ctx, env, *conn);
    } catch (const std::exception& e) {
      try {
        send_error(*conn, env, env.op, ERR_INVALID_REQUEST, e.what());
      } catch (...) {
      }
    }
  }

  cleanup_context(ctx);

  // Unregister and clear emit_event to prevent dangling references to conn.
  session_contexts_.wlock()->erase(ctx.session_id.id);
}

// ── Envelope helpers
// ──────────────────────────────────────────────────────────

/**
 * @brief Send a protocol response envelope.
 */
void server::send_response(
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
}

/**
 * @brief Send an error response envelope.
 */
void server::send_error(
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
}

// ── Request dispatch
// ──────────────────────────────────────────────────────────

/**
 * @brief Validate and route one request envelope to its operation handler.
 */
void server::handle_request(
    context& ctx,
    const shared::envelope& env,
    transport::server_connection_iface& conn) {
  if (env.op == 0u) {
    send_error(
        conn, env, OP_CONNECT, ERR_INVALID_REQUEST, "Missing operation token");
    return;
  }

  if (env.kind != KIND_REQUEST) {
    send_error(
        conn,
        env,
        OP_CONNECT,
        ERR_INVALID_REQUEST,
        "Unexpected non-request envelope");
    return;
  }

  int32_t version = env.version;
  if (version != PROTOCOL_VERSION) {
    send_error(
        conn,
        env,
        OP_CONNECT,
        ERR_UNSUPPORTED_VERSION,
        "Unsupported protocol version");
    return;
  }

  bison::key_t op = env.op;

  if (op == OP_CONNECT)
    handle_connect(ctx, env, conn);
  else if (op == OP_DESCRIBE)
    handle_describe(ctx, env, conn);
  else if (op == OP_INSTANTIATE)
    handle_instantiate(ctx, env, conn);
  else if (op == OP_CLEAR)
    handle_clear(ctx, env, conn);
  else if (op == OP_SET)
    handle_set(ctx, env, conn);
  else if (op == OP_GET)
    handle_get(ctx, env, conn);
  else if (op == OP_CALL)
    handle_call(ctx, env, conn);
  else if (op == OP_DESTROY)
    handle_destroy(ctx, env, conn);
  else if (op == OP_DISCONNECT)
    handle_disconnect(ctx, env, conn);
  else
    send_error(conn, env, op, ERR_UNKNOWN_OPERATION, "Unknown operation");
}

// ── Operation handlers
// ────────────────────────────────────────────────────────

/** @brief Handle `connect` handshake requests. */
void server::handle_connect(
    context& /*ctx*/,
    const shared::envelope& env,
    transport::server_connection_iface& conn) {
  bison::dynamic resp;
  resp[FIELD_VERSION] = int32_t{PROTOCOL_VERSION};
  send_response(conn, env, OP_CONNECT, std::move(resp));
}

/** @brief Handle class metadata requests. */
void server::handle_describe(
    context& /*ctx*/,
    const shared::envelope& env,
    transport::server_connection_iface& conn) {
  const auto& p = env.payload;

  bison::key_t requested = p.as<bison::key_t>(FIELD_KLASS);
  bison::dynamic resp;

  {
    auto lp = bison::dynamic::getRegistry().rlock();
    auto& classes = *lp;

    if (static_cast<bison::hash_t>(requested) == 0u) {
      size_t idx = 0;
      for (const auto& [klass, proto] : classes) {
        if (klass == CLASS_ENVELOPE)
          continue;
        bison::dynamic desc;
        desc[FIELD_KLASS] = klass;
        resp[idx++] = bison::dynamic_ptr{std::move(desc)};
      }
    } else {
      auto it = classes.find(requested);
      if (it == classes.end()) {
        lp.unlock();
        send_error(
            conn, env, OP_DESCRIBE, ERR_CLASS_NOT_FOUND, "Class not found");
        return;
      }
      resp[FIELD_KLASS] = requested;
      it->second->forEach(
          [&resp](bison::key_t k, const bison::field& v) { resp[k] = v; });
    }
  }

  send_response(conn, env, OP_DESCRIBE, std::move(resp));
}

/** @brief Handle server-side object instantiation requests. */
void server::handle_instantiate(
    context& ctx,
    const shared::envelope& env,
    transport::server_connection_iface& conn) {
  const auto& p = env.payload;

  bison::key_t klass = p.as<bison::key_t>(FIELD_KLASS);

  {
    auto lp = bison::dynamic::getRegistry().rlock();
    if (!lp->count(klass)) {
      send_error(
          conn,
          env,
          OP_INSTANTIATE,
          ERR_CLASS_NOT_FOUND,
          "Class not registered on server");
      return;
    }
  }

  auto obj =
      std::make_shared<bison::dynamic>(bison::dynamic::instantiate(klass));

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
      send_error(
          conn,
          env,
          OP_INSTANTIATE,
          ERR_INTERNAL_ERROR,
          std::string("__construct failed: ") + e.what());
      return;
    }
  }

  const bison::key_t oid = shared::generate_id();
  ctx.objects[oid.id] = obj;

  bison::dynamic resp;
  resp[FIELD_OBJECT_ID] = oid;
  resp[FIELD_KLASS] = klass;
  send_response(conn, env, OP_INSTANTIATE, std::move(resp));
}

/** @brief Handle clear requests for a live remote object. */
void server::handle_clear(
    context& ctx,
    const shared::envelope& env,
    transport::server_connection_iface& conn) {
  bison::key_t oid = env.object_id;
  auto it = ctx.objects.find(oid.id);
  if (it == ctx.objects.end()) {
    send_error(conn, env, OP_CLEAR, ERR_OBJECT_NOT_FOUND, "Object not found");
    return;
  }

  bison::key_t klass_key = it->second->as<bison::key_t>(bison::dynamic::CLASS);
  {
    auto lp = bison::dynamic::getRegistry().rlock();
    auto class_it = lp->find(klass_key);
    if (class_it != lp->end() && class_it->second) {
      *it->second = class_it->second->clone();
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

  send_response(conn, env, OP_CLEAR, bison::dynamic{});
}

/** @brief Handle partial field updates for a live remote object. */
void server::handle_set(
    context& ctx,
    const shared::envelope& env,
    transport::server_connection_iface& conn) {
  bison::key_t oid = env.object_id;
  auto it = ctx.objects.find(oid.id);
  if (it == ctx.objects.end()) {
    send_error(conn, env, OP_SET, ERR_OBJECT_NOT_FOUND, "Object not found");
    return;
  }
  auto& obj = *it->second;

  bison::dynamic patch = env.payload.clone();

  if (obj.findMethod(HOOK_SETTER) != nullptr) {
    try {
      patch = obj.call(HOOK_SETTER, patch);
    } catch (const std::exception& e) {
      send_error(
          conn,
          env,
          OP_SET,
          ERR_INTERNAL_ERROR,
          std::string("__setter failed: ") + e.what());
      return;
    }
  }

  patch.forEach([&obj](bison::key_t k, const bison::field& v) {
    if (k != bison::dynamic::CLASS && k != bison::dynamic::PARENT)
      obj[k] = v;
  });

  send_response(conn, env, OP_SET, bison::dynamic{});
}

/** @brief Handle object reads with optional projection payload. */
void server::handle_get(
    context& ctx,
    const shared::envelope& env,
    transport::server_connection_iface& conn) {
  bison::key_t oid = env.object_id;
  auto it = ctx.objects.find(oid.id);
  if (it == ctx.objects.end()) {
    send_error(conn, env, OP_GET, ERR_OBJECT_NOT_FOUND, "Object not found");
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
    result = obj.clone();
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

  send_response(conn, env, OP_GET, std::move(result));
}

/** @brief Handle method invocation requests. */
void server::handle_call(
    context& ctx,
    const shared::envelope& env,
    transport::server_connection_iface& conn) {
  bison::key_t oid = env.object_id;
  auto it = ctx.objects.find(oid.id);
  if (it == ctx.objects.end()) {
    send_error(conn, env, OP_CALL, ERR_OBJECT_NOT_FOUND, "Object not found");
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
      send_response(conn, env, OP_CALL, std::move(res));
  } catch (const std::exception& e) {
    if (!oneway)
      send_error(conn, env, OP_CALL, ERR_INTERNAL_ERROR, e.what());
  }
}

/** @brief Handle explicit object destruction requests. */
void server::handle_destroy(
    context& ctx,
    const shared::envelope& env,
    transport::server_connection_iface& conn) {
  bison::key_t oid = env.object_id;
  auto it = ctx.objects.find(oid.id);
  if (it == ctx.objects.end()) {
    send_error(conn, env, OP_DESTROY, ERR_OBJECT_NOT_FOUND, "Object not found");
    return;
  }

  if (it->second->findMethod(HOOK_DESTRUCT) != nullptr) {
    try {
      it->second->call(HOOK_DESTRUCT, bison::dynamic{});
    } catch (...) {
    }
  }

  ctx.objects.erase(it);
  send_response(conn, env, OP_DESTROY, bison::dynamic{});
}

/** @brief Handle disconnect requests and close the connection context. */
void server::handle_disconnect(
    context& ctx,
    const shared::envelope& env,
    transport::server_connection_iface& conn) {
  cleanup_context(ctx);
  conn.close();
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
  ctx.emit_event = nullptr;
}

} // namespace bdg::bison::rmi
