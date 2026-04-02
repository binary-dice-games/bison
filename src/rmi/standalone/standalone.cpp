// MIT License © 2025 Binary Dice Games
/**
 * @file standalone.cpp
 * @brief Implementation of the standalone in-process RMI runtime.
 *
 * All operations execute synchronously on the calling thread.  Object
 * references are stored directly in the session context (`context::objects`)
 * and dispatched without envelope serialization or transport I/O.
 */
#include "src/rmi/standalone/standalone.hpp"

#include "src/rmi/shared/constants.hpp"
#include "src/rmi/shared/ids.hpp"

#include <stdexcept>

namespace bdg::bison::rmi {

using namespace shared::constants;

namespace {

/**
 * @brief Read a key token from a dynamic field (accepts `key_t` or `hash_t`).
 * @param obj        Source object.
 * @param field_name Field key to read.
 * @return Parsed key token.
 * @throws std::runtime_error if the field is missing or has an incompatible
 *         type.
 */
bison::key_t read_key_token(
    const bison::dynamic& obj,
    bison::key_t field_name) {
  const auto* f = obj.findField(field_name);
  if (f == nullptr)
    throw std::runtime_error("Missing key token field");
  if (f->is<bison::key_t>())
    return f->as<bison::key_t>();
  if (f->is<bison::hash_t>())
    return bison::key_t{f->as<bison::hash_t>()};
  throw std::runtime_error("Invalid key token type");
}

} // namespace

// ── Lifecycle ──────────────────────────────────────────────────────────────

/** @brief Initialise the session context with an event-dispatch callback. */
standalone::standalone() {
  ctx_.session_id = shared::generate_id();
  ctx_.emit_event =
      [this](bison::key_t object_id, bison::key_t name, bison::dynamic params) {
        auto obj_it = event_handlers_.find(object_id.id);
        if (obj_it == event_handlers_.end())
          return;
        auto ev_it = obj_it->second.find(static_cast<bison::hash_t>(name));
        if (ev_it == obj_it->second.end())
          return;
        try {
          ev_it->second(std::move(params));
        } catch (...) {
          // Silently discard exceptions from event handlers to maintain
          // consistency with the server-side event-dispatch behaviour.
        }
      };
}

/** @brief Invoke `__destruct` on all remaining objects and clear the context.
 */
standalone::~standalone() {
  for (auto& [oid, obj] : ctx_.objects) {
    if (obj && obj->findMethod(HOOK_DESTRUCT) != nullptr) {
      try {
        obj->call(HOOK_DESTRUCT, bison::dynamic{});
      } catch (...) {
      }
    }
  }
  ctx_.objects.clear();
  event_handlers_.clear();
}

// ── Client-compatible public methods ──────────────────────────────────────

/** @copydoc bdg::bison::rmi::standalone::connect */
void standalone::connect(bison::dynamic /*params*/) {
  // No transport to open; standalone is always "connected".
}

/** @copydoc bdg::bison::rmi::standalone::describe */
std::future<bison::dynamic> standalone::describe(bison::key_t klass) {
  try {
    return resolved(handle_describe(klass));
  } catch (...) {
    std::promise<bison::dynamic> p;
    p.set_exception(std::current_exception());
    return p.get_future();
  }
}

/** @copydoc bdg::bison::rmi::standalone::instantiate */
std::future<proxy::dynamic> standalone::instantiate(
    bison::key_t klass,
    bison::dynamic params) {
  std::promise<proxy::dynamic> p;
  try {
    bison::dynamic resp = handle_instantiate(klass, std::move(params));
    bison::key_t oid = resp.as<bison::key_t>(FIELD_OBJECT_ID);
    p.set_value(proxy::dynamic{this, oid});
  } catch (...) {
    p.set_exception(std::current_exception());
  }
  return p.get_future();
}

/** @copydoc bdg::bison::rmi::standalone::destroy */
void standalone::destroy(proxy::dynamic&& proxy) {
  if (!proxy.valid())
    return;

  bison::key_t oid = proxy.object_id();
  proxy.valid_ = false;
  proxy.backend_ = nullptr;

  auto it = ctx_.objects.find(oid.id);
  if (it == ctx_.objects.end())
    return;

  if (it->second && it->second->findMethod(HOOK_DESTRUCT) != nullptr) {
    try {
      it->second->call(HOOK_DESTRUCT, bison::dynamic{});
    } catch (...) {
    }
  }

  ctx_.objects.erase(it);
  unregister_object_events(oid);
}

/** @copydoc bdg::bison::rmi::standalone::disconnect */
void standalone::disconnect() {
  // No transport to close; standalone is always "connected".
}

/** @copydoc bdg::bison::rmi::standalone::send_request */
std::future<bison::dynamic> standalone::send_request(
    bison::key_t op,
    bison::key_t object_id,
    bison::dynamic payload,
    bool oneway) {
  try {
    bison::dynamic result;

    if (op == OP_CLEAR) {
      result = handle_clear(object_id);
    } else if (op == OP_SET) {
      result = handle_set(object_id, std::move(payload));
    } else if (op == OP_GET) {
      result = handle_get(object_id, std::move(payload));
    } else if (op == OP_CALL) {
      result = handle_call(object_id, std::move(payload), oneway);
    } else if (op == OP_DESTROY) {
      result = handle_destroy(object_id);
    } else {
      throw std::runtime_error("Unsupported operation in standalone mode");
    }

    return resolved(std::move(result));
  } catch (...) {
    std::promise<bison::dynamic> p;
    p.set_exception(std::current_exception());
    return p.get_future();
  }
}

/** @copydoc bdg::bison::rmi::standalone::register_event_handler */
void standalone::register_event_handler(
    bison::key_t object_id,
    bison::key_t name,
    std::function<void(bison::dynamic)> handler) {
  event_handlers_[object_id.id][static_cast<bison::hash_t>(name)] =
      std::move(handler);
}

/** @copydoc bdg::bison::rmi::standalone::unregister_object_events */
void standalone::unregister_object_events(bison::key_t object_id) {
  event_handlers_.erase(object_id.id);
}

// ── Private operation handlers ────────────────────────────────────────────

/**
 * @brief Return metadata for one or all registered classes.
 *
 * Mirrors `server::handle_describe` without transport.
 */
bison::dynamic standalone::handle_describe(bison::key_t klass) {
  bison::dynamic resp;
  auto lp = bison::dynamic::getRegistry().rlock();
  const auto& classes = *lp;

  if (static_cast<bison::hash_t>(klass) == 0u) {
    std::size_t idx = 0;
    for (const auto& [k, proto] : classes) {
      if (k == CLASS_ENVELOPE)
        continue;
      bison::dynamic desc;
      desc[FIELD_KLASS] = k;
      resp[idx++] = bison::dynamic_ptr{std::move(desc)};
    }
  } else {
    auto it = classes.find(klass);
    if (it == classes.end())
      throw std::runtime_error("Class not found");
    resp[FIELD_KLASS] = klass;
    it->second->forEach(
        [&resp](bison::key_t k, const bison::field& v) { resp[k] = v; });
  }

  return resp;
}

/**
 * @brief Create and store a new object instance.
 *
 * Mirrors `server::handle_instantiate` without transport.  Returns the
 * response payload containing the new object ID.
 */
bison::dynamic standalone::handle_instantiate(
    bison::key_t klass,
    bison::dynamic params) {
  {
    auto lp = bison::dynamic::getRegistry().rlock();
    if (!lp->count(klass))
      throw std::runtime_error("Class not registered");
  }

  auto obj =
      std::make_shared<bison::dynamic>(bison::dynamic::instantiate(klass));

  if (obj->findMethod(HOOK_CONSTRUCT) != nullptr) {
    obj->call(HOOK_CONSTRUCT, params);
  }

  const bison::key_t oid = shared::generate_id();
  ctx_.objects[oid.id] = obj;

  bison::dynamic resp;
  resp[FIELD_OBJECT_ID] = oid;
  resp[FIELD_KLASS] = klass;
  return resp;
}

/**
 * @brief Reset an object to its prototype defaults.
 *
 * Mirrors `server::handle_clear` without transport.
 */
bison::dynamic standalone::handle_clear(bison::key_t object_id) {
  auto& obj_ptr = require_object(object_id);
  auto& obj = *obj_ptr;

  bison::key_t klass_key = obj.as<bison::key_t>(bison::dynamic::CLASS);
  {
    auto lp = bison::dynamic::getRegistry().rlock();
    auto class_it = lp->find(klass_key);
    if (class_it != lp->end() && class_it->second) {
      obj = class_it->second->clone();
    } else {
      obj = bison::dynamic::instantiate(klass_key);
    }
  }

  if (obj.findMethod(HOOK_CLEAR) != nullptr) {
    try {
      obj.call(HOOK_CLEAR, bison::dynamic{});
    } catch (...) {
    }
  }

  return bison::dynamic{};
}

/**
 * @brief Apply a partial field update to a stored object.
 *
 * Mirrors `server::handle_set` without transport.
 */
bison::dynamic standalone::handle_set(
    bison::key_t object_id,
    bison::dynamic payload) {
  auto& obj_ptr = require_object(object_id);
  auto& obj = *obj_ptr;

  bison::dynamic patch = payload.clone();

  if (obj.findMethod(HOOK_SETTER) != nullptr) {
    patch = obj.call(HOOK_SETTER, patch);
  }

  patch.forEach([&obj](bison::key_t k, const bison::field& v) {
    if (k != bison::dynamic::CLASS && k != bison::dynamic::PARENT)
      obj[k] = v;
  });

  return bison::dynamic{};
}

/**
 * @brief Retrieve fields from a stored object with optional projection.
 *
 * Mirrors `server::handle_get` without transport.
 */
bison::dynamic standalone::handle_get(
    bison::key_t object_id,
    bison::dynamic projection) {
  auto& obj_ptr = require_object(object_id);
  const auto& obj = *obj_ptr;

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
      const auto* f = obj.findField(k);
      if (f)
        result[k] = *f;
    });
  }

  if (obj.findMethod(HOOK_GETTER) != nullptr) {
    try {
      result = const_cast<bison::dynamic&>(obj).call(HOOK_GETTER, result);
    } catch (...) {
    }
  }

  return result;
}

/**
 * @brief Invoke a method on a stored object.
 *
 * Mirrors `server::handle_call` without transport.
 */
bison::dynamic standalone::handle_call(
    bison::key_t object_id,
    bison::dynamic payload,
    bool oneway) {
  auto& obj_ptr = require_object(object_id);
  auto& obj = *obj_ptr;

  bison::key_t method_name = read_key_token(payload, FIELD_NAME);

  bison::dynamic args;
  auto& params_field = payload[FIELD_PARAMS];
  if (params_field.is<bison::dynamic_ptr>()) {
    auto ptr = params_field.as<bison::dynamic_ptr>();
    if (ptr)
      args = std::move(*ptr);
  }

  bison::dynamic res = obj.call(method_name, args);
  return oneway ? bison::dynamic{} : std::move(res);
}

/**
 * @brief Destroy a stored object and invoke its `__destruct` hook.
 *
 * Mirrors `server::handle_destroy` without transport.
 */
bison::dynamic standalone::handle_destroy(bison::key_t object_id) {
  auto it = ctx_.objects.find(object_id.id);
  if (it == ctx_.objects.end())
    throw std::runtime_error("Object not found");

  if (it->second && it->second->findMethod(HOOK_DESTRUCT) != nullptr) {
    try {
      it->second->call(HOOK_DESTRUCT, bison::dynamic{});
    } catch (...) {
    }
  }

  ctx_.objects.erase(it);
  unregister_object_events(object_id);
  return bison::dynamic{};
}

// ── Helpers ───────────────────────────────────────────────────────────────

/** @copydoc bdg::bison::rmi::standalone::require_object */
bison::dynamic_ptr& standalone::require_object(bison::key_t object_id) {
  auto it = ctx_.objects.find(object_id.id);
  if (it == ctx_.objects.end())
    throw std::runtime_error("Object not found");
  return it->second;
}

/** @brief Wrap @p value in an already-resolved `std::future`. */
std::future<bison::dynamic> standalone::resolved(bison::dynamic value) {
  std::promise<bison::dynamic> p;
  p.set_value(std::move(value));
  return p.get_future();
}

} // namespace bdg::bison::rmi
