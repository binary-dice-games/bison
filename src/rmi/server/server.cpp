// MIT License © 2025 Binary Dice Games
#include "src/rmi/server/server.hpp"

#include "src/rmi/shared/constants.hpp"
#include "src/rmi/shared/ids.hpp"

#include <shared_mutex>
#include <stdexcept>

namespace bdg::bison::rmi {

using namespace shared::constants;

// ── Lifecycle ─────────────────────────────────────────────────────────────────

server::~server() {
  if (running_.load()) { try { stop(); } catch (...) {} }
}

void server::listen(bison::dynamic params) {
  shared::register_envelope();
  running_.store(true);
  transport_->start(std::move(params));
  accept_thread_ = std::thread(&server::accept_loop, this);
}

void server::stop() {
  running_.store(false);
  transport_->stop();
  if (accept_thread_.joinable()) accept_thread_.join();
  join_workers();
}

void server::join_workers() {
  std::lock_guard<std::mutex> lk(workers_mutex_);
  for (auto& t : workers_) if (t.joinable()) t.join();
  workers_.clear();
}

// ── Accept loop ───────────────────────────────────────────────────────────────

void server::accept_loop() {
  while (running_.load(std::memory_order_acquire)) {
    auto conn = transport_->accept(std::chrono::milliseconds{100});
    if (!conn) continue;
    std::lock_guard<std::mutex> lk(workers_mutex_);
    workers_.emplace_back(
        std::thread(&server::client_worker, this, std::move(conn)));
  }
}

// ── Client worker ─────────────────────────────────────────────────────────────

void server::client_worker(std::unique_ptr<connection_iface> conn) {
  context ctx;
  ctx.emit_event = [&conn](const std::string& oid, bison::key_t name,
                            bison::dynamic params) {
    bison::dynamic ev_payload;
    ev_payload[FIELD_NAME]   = name;
    ev_payload[FIELD_PARAMS] = std::make_shared<bison::dynamic>(std::move(params));
    const std::string pb     = shared::encode_payload(ev_payload);
    auto frame = shared::encode_envelope(KIND_EVENT, OP_EVENT,
                                         {}, oid, true, pb);
    conn->send(std::move(frame));
  };

  while (!conn->is_closed()) {
    std::vector<char> frame;
    if (!conn->receive(frame, std::chrono::milliseconds{50})) continue;
    try {
      auto env = shared::decode_envelope(frame);
      handle_request(ctx, *env, *conn);
    } catch (const std::exception& e) {
      try {
        auto err_bytes = shared::encode_error(ERR_INVALID_REQUEST, e.what());
        conn->send(shared::encode_envelope(KIND_RESPONSE, OP_CONNECT,
                                           {}, {}, false, {}, err_bytes));
      } catch (...) {}
    }
  }

  cleanup_context(ctx);
}

// ── Envelope helpers ──────────────────────────────────────────────────────────

void server::send_response(connection_iface&     conn,
                            const bison::dynamic& env,
                            bison::key_t          op,
                            const std::string&    payload_bytes,
                            const std::string&    error_bytes) {
  std::string request_id = env.as<std::string>(FIELD_REQUEST_ID);
  std::string object_id  = env.as<std::string>(FIELD_OBJECT_ID);
  conn.send(shared::encode_envelope(KIND_RESPONSE, op, request_id,
                                    object_id, false,
                                    payload_bytes, error_bytes));
}

void server::send_error(connection_iface&     conn,
                         const bison::dynamic& env,
                         bison::key_t          op,
                         bison::key_t          code,
                         const std::string&    message) {
  send_response(conn, env, op, {}, shared::encode_error(code, message));
}

// ── Request dispatch ──────────────────────────────────────────────────────────

void server::handle_request(context& ctx, const bison::dynamic& env,
                              connection_iface& conn) {
  int32_t version = env.as<int32_t>(FIELD_VERSION);
  if (version != PROTOCOL_VERSION) {
    send_error(conn, env, OP_CONNECT, ERR_UNSUPPORTED_VERSION,
               "Unsupported protocol version");
    return;
  }

  bison::key_t op = env.as<bison::key_t>(FIELD_OP);

  if      (op == OP_CONNECT)     handle_connect(ctx, env, conn);
  else if (op == OP_DESCRIBE)    handle_describe(ctx, env, conn);
  else if (op == OP_INSTANTIATE) handle_instantiate(ctx, env, conn);
  else if (op == OP_CLEAR)       handle_clear(ctx, env, conn);
  else if (op == OP_SET)         handle_set(ctx, env, conn);
  else if (op == OP_GET)         handle_get(ctx, env, conn);
  else if (op == OP_CALL)        handle_call(ctx, env, conn);
  else if (op == OP_DESTROY)     handle_destroy(ctx, env, conn);
  else if (op == OP_DISCONNECT)  handle_disconnect(ctx, env, conn);
  else
    send_error(conn, env, op, ERR_UNKNOWN_OPERATION, "Unknown operation");
}

// ── Operation handlers ────────────────────────────────────────────────────────

void server::handle_connect(context& /*ctx*/, const bison::dynamic& env,
                              connection_iface& conn) {
  bison::dynamic resp;
  resp[FIELD_VERSION] = int32_t{PROTOCOL_VERSION};
  send_response(conn, env, OP_CONNECT, shared::encode_payload(resp));
}

void server::handle_describe(context& /*ctx*/, const bison::dynamic& env,
                               connection_iface& conn) {
  std::string pb = env.as<std::string>(FIELD_PAYLOAD);
  auto        payload = shared::decode_payload(pb);

  bison::key_t requested = payload->as<bison::key_t>(FIELD_KLASS);
  bison::dynamic resp;

  std::shared_lock<std::shared_mutex> lk(bison::dynamic::getMutex());
  auto& classes = bison::dynamic::getClasses();

  if (static_cast<bison::hash_t>(requested) == 0u) {
    size_t idx = 0;
    for (const auto& [klass, proto] : classes) {
      if (klass == CLASS_ENVELOPE) continue;
      bison::dynamic desc;
      desc[FIELD_KLASS] = klass;
      resp[idx++] = std::make_shared<bison::dynamic>(std::move(desc));
    }
  } else {
    auto it = classes.find(requested);
    if (it == classes.end()) {
      lk.unlock();
      send_error(conn, env, OP_DESCRIBE, ERR_CLASS_NOT_FOUND, "Class not found");
      return;
    }
    resp[FIELD_KLASS] = requested;
    it->second->forEach([&resp](bison::key_t k, const bison::field& v) {
      resp[k] = v;
    });
  }

  send_response(conn, env, OP_DESCRIBE, shared::encode_payload(resp));
}

void server::handle_instantiate(context& ctx, const bison::dynamic& env,
                                  connection_iface& conn) {
  std::string pb      = env.as<std::string>(FIELD_PAYLOAD);
  auto        payload = shared::decode_payload(pb);

  bison::key_t klass = payload->as<bison::key_t>(FIELD_KLASS);

  {
    std::shared_lock<std::shared_mutex> lk(bison::dynamic::getMutex());
    if (!bison::dynamic::getClasses().count(klass)) {
      send_error(conn, env, OP_INSTANTIATE, ERR_CLASS_NOT_FOUND,
                 "Class not registered on server");
      return;
    }
  }

  auto obj = std::make_shared<bison::dynamic>(bison::dynamic::instantiate(klass));

  if (obj->findMethod(HOOK_CONSTRUCT) != nullptr) {
    try {
      bison::dynamic construct_params;
      auto& pf = (*payload)[FIELD_PARAMS];
      if (pf.is<std::shared_ptr<bison::dynamic>>()) {
        auto ptr = static_cast<std::shared_ptr<bison::dynamic>>(pf);
        if (ptr) construct_params = std::move(*ptr);
      }
      obj->call(HOOK_CONSTRUCT, construct_params);
    } catch (const std::exception& e) {
      send_error(conn, env, OP_INSTANTIATE, ERR_INTERNAL_ERROR,
                 std::string("__construct failed: ") + e.what());
      return;
    }
  }

  const std::string oid = shared::generate_id();
  ctx.objects[oid] = obj;

  bison::dynamic resp;
  resp[FIELD_OBJECT_ID] = oid;
  resp[FIELD_KLASS]     = klass;
  send_response(conn, env, OP_INSTANTIATE, shared::encode_payload(resp));
}

void server::handle_clear(context& ctx, const bison::dynamic& env,
                            connection_iface& conn) {
  std::string oid = env.as<std::string>(FIELD_OBJECT_ID);
  auto it = ctx.objects.find(oid);
  if (it == ctx.objects.end()) {
    send_error(conn, env, OP_CLEAR, ERR_OBJECT_NOT_FOUND, "Object not found");
    return;
  }

  bison::key_t klass_key = it->second->as<bison::key_t>(bison::dynamic::CLASS);
  *it->second = bison::dynamic::instantiate(klass_key);

  if (it->second->findMethod(HOOK_CLEAR) != nullptr) {
    try { it->second->call(HOOK_CLEAR, bison::dynamic{}); } catch (...) {}
  }

  send_response(conn, env, OP_CLEAR, {});
}

void server::handle_set(context& ctx, const bison::dynamic& env,
                          connection_iface& conn) {
  std::string oid = env.as<std::string>(FIELD_OBJECT_ID);
  auto it = ctx.objects.find(oid);
  if (it == ctx.objects.end()) {
    send_error(conn, env, OP_SET, ERR_OBJECT_NOT_FOUND, "Object not found");
    return;
  }
  auto& obj = *it->second;

  std::string pb    = env.as<std::string>(FIELD_PAYLOAD);
  auto        patch = shared::decode_payload(pb);

  if (obj.findMethod(HOOK_SETTER) != nullptr) {
    try { *patch = obj.call(HOOK_SETTER, *patch); }
    catch (const std::exception& e) {
      send_error(conn, env, OP_SET, ERR_INTERNAL_ERROR,
                 std::string("__setter failed: ") + e.what());
      return;
    }
  }

  patch->forEach([&obj](bison::key_t k, const bison::field& v) {
    if (k != bison::dynamic::CLASS && k != bison::dynamic::PARENT)
      obj[k] = v;
  });

  send_response(conn, env, OP_SET, {});
}

void server::handle_get(context& ctx, const bison::dynamic& env,
                          connection_iface& conn) {
  std::string oid = env.as<std::string>(FIELD_OBJECT_ID);
  auto it = ctx.objects.find(oid);
  if (it == ctx.objects.end()) {
    send_error(conn, env, OP_GET, ERR_OBJECT_NOT_FOUND, "Object not found");
    return;
  }
  auto& obj = *it->second;

  std::string pb         = env.as<std::string>(FIELD_PAYLOAD);
  auto        projection = shared::decode_payload(pb);

  bool has_projection = false;
  projection->forEach([&](bison::key_t k, const bison::field&) {
    if (k != bison::dynamic::CLASS && k != bison::dynamic::PARENT)
      has_projection = true;
  });

  bison::dynamic result;
  if (!has_projection) {
    result = obj.clone();
  } else {
    projection->forEach([&](bison::key_t k, const bison::field&) {
      if (k == bison::dynamic::CLASS || k == bison::dynamic::PARENT) return;
      auto* f = obj.findField(k);
      if (f) result[k] = *f;
    });
  }

  if (obj.findMethod(HOOK_GETTER) != nullptr) {
    try { result = obj.call(HOOK_GETTER, result); } catch (...) {}
  }

  send_response(conn, env, OP_GET, shared::encode_payload(result));
}

void server::handle_call(context& ctx, const bison::dynamic& env,
                           connection_iface& conn) {
  std::string oid = env.as<std::string>(FIELD_OBJECT_ID);
  auto it = ctx.objects.find(oid);
  if (it == ctx.objects.end()) {
    send_error(conn, env, OP_CALL, ERR_OBJECT_NOT_FOUND, "Object not found");
    return;
  }
  auto& obj = *it->second;

  std::string pb     = env.as<std::string>(FIELD_PAYLOAD);
  auto        params = shared::decode_payload(pb);

  bison::key_t method_name = params->as<bison::key_t>(FIELD_NAME);
  bool         oneway      = env.as<bool>(FIELD_ONEWAY);

  try {
    bison::dynamic res = obj.call(method_name, *params);
    if (!oneway) send_response(conn, env, OP_CALL, shared::encode_payload(res));
  } catch (const std::exception& e) {
    if (!oneway) send_error(conn, env, OP_CALL, ERR_INTERNAL_ERROR, e.what());
  }
}

void server::handle_destroy(context& ctx, const bison::dynamic& env,
                              connection_iface& conn) {
  std::string oid = env.as<std::string>(FIELD_OBJECT_ID);
  auto it = ctx.objects.find(oid);
  if (it == ctx.objects.end()) {
    send_error(conn, env, OP_DESTROY, ERR_OBJECT_NOT_FOUND, "Object not found");
    return;
  }

  if (it->second->findMethod(HOOK_DESTRUCT) != nullptr) {
    try { it->second->call(HOOK_DESTRUCT, bison::dynamic{}); } catch (...) {}
  }

  ctx.objects.erase(it);
  send_response(conn, env, OP_DESTROY, {});
}

void server::handle_disconnect(context& ctx, const bison::dynamic& env,
                                 connection_iface& conn) {
  cleanup_context(ctx);
  conn.close();
}

void server::cleanup_context(context& ctx) {
  for (auto& [oid, obj] : ctx.objects) {
    if (obj && obj->findMethod(HOOK_DESTRUCT) != nullptr) {
      try { obj->call(HOOK_DESTRUCT, bison::dynamic{}); } catch (...) {}
    }
  }
  ctx.objects.clear();
}

} // namespace bdg::bison::rmi
