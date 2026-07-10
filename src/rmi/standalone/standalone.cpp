// MIT License © 2025 Binary Dice Games
/**
 * @file standalone.cpp
 * @brief Implementation of the standalone in-process RMI runtime.
 *
 * All proxy operations are queued to a background worker thread and return
 * pending futures.  Object references are stored in the session context
 * (`ctx_`) and dispatched without envelope serialization or transport I/O.
 */
#include "src/rmi/standalone/standalone.hpp"

#include "src/rmi/shared/constants.hpp"
#include "src/rmi/shared/ids.hpp"

#include <sstream>
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
bison::key_t read_key_token(const bison::dynamic& obj, bison::key_t field_name) {
  const auto* f = obj.findField(field_name);
  if (f == nullptr)
    throw std::runtime_error("Missing key token field");
  if (f->is<bison::key_t>())
    return f->as<bison::key_t>();
  if (f->is<bison::hash_t>())
    return bison::key_t{f->as<bison::hash_t>()};
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

// ── Lifecycle ──────────────────────────────────────────────────────────────

/** @brief Initialise the session context and start the worker thread. */
standalone::standalone() {
  {
    auto lp = ctx_.wlock();
    lp->session_id = shared::generate_id();
    lp->emit_event = [this](bison::key_t object_id, bison::key_t name, bison::dynamic params) {
      // Copy the handler out of the map before calling it so we do not
      // hold event_handlers_ lock while the user callback runs.
      std::function<void(bison::dynamic)> handler;
      {
        auto elp = event_handlers_.rlock();
        auto obj_it = elp->find(object_id.id);
        if (obj_it == elp->end())
          return;
        auto ev_it = obj_it->second.find(static_cast<bison::hash_t>(name));
        if (ev_it == obj_it->second.end())
          return;
        handler = ev_it->second;
      }
      try {
        handler(std::move(params));
      } catch (...) {
        // Silently discard exceptions from event handlers to maintain
        // consistency with server-side event-dispatch behaviour.
      }
    };
  }

  running_.store(true, std::memory_order_release);
  worker_ = std::thread(&standalone::worker_loop, this);
}

/**
 * @brief Stop the worker, invoke `__destruct` on remaining objects, and
 *        clear the context.
 */
standalone::~standalone() {
  stop_worker();

  // Worker is joined; we are the only thread — cleanup remaining objects.
  auto lp = ctx_.wlock();
  for (auto& [oid, obj] : lp->objects) {
    if (obj && obj->findMethod(HOOK_DESTRUCT) != nullptr) {
      try {
        obj->call(HOOK_DESTRUCT, bison::dynamic{});
      } catch (...) {
      }
    }
  }
  lp->objects.clear();
}

// ── Client-compatible public methods ──────────────────────────────────────

/** @copydoc bdg::bison::rmi::standalone::connect */
void standalone::connect(bison::dynamic /*params*/) {
  // Worker already running from constructor. Fire on_session_created()
  // exactly once, with ctx_'s lock released first so the hook may safely
  // call back into instantiate()/other operations without deadlocking
  // against the single worker thread.
  if (!session_created_.exchange(true, std::memory_order_acq_rel)) {
    context* ctx_ptr;
    {
      auto lp = ctx_.wlock();
      ctx_ptr = &*lp;
    }
    on_session_created(*ctx_ptr);
  }
}

/** @copydoc bdg::bison::rmi::standalone::describe */
std::future<bison::dynamic> standalone::describe(bison::key_t ns, bison::key_t klass) {
  return enqueue([this, ns, klass]() -> bison::dynamic {
    return dispatch([this, ns, klass]() -> bison::dynamic { return handle_describe(ns, klass); });
  });
}

std::future<proxy::dynamic> standalone::instantiate(bison::key_t ns, bison::key_t klass, bison::dynamic params) {
  auto f = enqueue([this, ns, klass, params = std::move(params)]() mutable -> bison::dynamic {
    return dispatch([this, ns, klass, &params]() -> bison::dynamic {
      return handle_instantiate(ns, klass, std::move(params));
    });
  });
  return std::async(std::launch::async, [this, f = std::move(f)]() mutable {
    auto result = f.get();
    bison::key_t oid = result.as<bison::key_t>(FIELD_OBJECT_ID);
    return proxy::dynamic{this, std::move(oid)};
  });
}

/** @copydoc bdg::bison::rmi::standalone::destroy */
void standalone::destroy(proxy::dynamic&& proxy) {
  if (!proxy.valid())
    return;

  bison::key_t oid = proxy.object_id();
  proxy.valid_ = false;
  proxy.backend_ = nullptr;
  destroy_object(oid);
}

/** @copydoc bdg::bison::rmi::standalone::destroy_object */
void standalone::destroy_object(bison::key_t object_id) {
  enqueue([this, object_id]() -> bison::dynamic {
    return dispatch([this, object_id]() -> bison::dynamic { return handle_destroy(object_id); });
  }).get();
}

/** @copydoc bdg::bison::rmi::standalone::disconnect */
void standalone::disconnect() {
  // Fire on_session_destroyed() exactly once, before stopping the worker,
  // with ctx_'s lock released first (same reasoning as connect()).
  if (!session_destroyed_.exchange(true, std::memory_order_acq_rel)) {
    context* ctx_ptr;
    {
      auto lp = ctx_.wlock();
      ctx_ptr = &*lp;
    }
    on_session_destroyed(*ctx_ptr);
  }
  stop_worker();
}

/** @copydoc bdg::bison::rmi::standalone::get_dictionary */
std::future<bison::dynamic> standalone::get_dictionary() {
  return enqueue([this]() -> bison::dynamic { return dispatch([this]() -> bison::dynamic { return handle_dictionary(); }); });
}

/** @copydoc bdg::bison::rmi::standalone::get_help */
std::future<bison::dynamic> standalone::get_help() {
  return enqueue([this]() -> bison::dynamic { return dispatch([this]() -> bison::dynamic { return handle_help(); }); });
}

/** @copydoc bdg::bison::rmi::standalone::send_request */
std::future<bison::dynamic>
standalone::send_request(bison::key_t op, bison::key_t object_id, bison::dynamic payload, bool oneway) {
  return enqueue([this, op, object_id, payload = std::move(payload), oneway]() mutable -> bison::dynamic {
    return dispatch([this, op, object_id, &payload, oneway]() -> bison::dynamic {
      if (op == OP_CLEAR) {
        return handle_clear(object_id);
      }
      if (op == OP_SET) {
        return handle_set(object_id, std::move(payload));
      }
      if (op == OP_GET) {
        return handle_get(object_id, std::move(payload));
      }
      if (op == OP_CALL) {
        return handle_call(object_id, std::move(payload), oneway);
      }
      if (op == OP_DESTROY) {
        return handle_destroy(object_id);
      }
      throw std::runtime_error("Unsupported operation in standalone mode");
    });
  });
}

/** @copydoc bdg::bison::rmi::standalone::register_event_handler */
void standalone::register_event_handler(
    bison::key_t object_id,
    bison::key_t name,
    std::function<void(bison::dynamic)> handler) {
  event_handlers_.wlock()->operator[](object_id.id)[static_cast<bison::hash_t>(name)] = std::move(handler);
}

/** @copydoc bdg::bison::rmi::standalone::unregister_object_events */
void standalone::unregister_object_events(bison::key_t object_id) {
  event_handlers_.wlock()->erase(object_id.id);
}

// ── Worker ────────────────────────────────────────────────────────────────

/** @brief Enqueue work and return a pending future. */
std::future<bison::dynamic> standalone::enqueue(std::function<bison::dynamic()> work) {
  if (!running_.load(std::memory_order_acquire)) {
    std::promise<bison::dynamic> p;
    p.set_exception(std::make_exception_ptr(std::runtime_error("standalone is not connected")));
    return p.get_future();
  }

  std::promise<bison::dynamic> promise;
  auto future = promise.get_future();
  queue_.withWLock([&](auto& q) { q.push({std::move(work), std::move(promise)}); });
  queue_.notify_one();
  return future;
}

/**
 * @brief Dequeue and execute tasks until stopped.
 *
 * When `running_` is cleared, the loop drains any remaining tasks before
 * exiting so that all pending futures are resolved.
 */
void standalone::worker_loop() {
  while (true) {
    queue_.wait([this](auto& q) { return !q.empty() || !running_.load(std::memory_order_relaxed); });

    task_item task;
    bool have_task = queue_.withWLock([&](auto& q) {
      if (q.empty())
        return false;
      task = std::move(q.front());
      q.pop();
      return true;
    });

    if (!have_task) {
      if (!running_.load(std::memory_order_relaxed))
        break;
      continue;
    }

    try {
      task.promise.set_value(task.work());
    } catch (...) {
      task.promise.set_exception(std::current_exception());
    }
  }
}

/** @brief Signal the worker to stop, drain remaining tasks, and join. */
void standalone::stop_worker() {
  if (!running_.load(std::memory_order_acquire))
    return;
  queue_.withWLock([this](auto&) { running_.store(false, std::memory_order_release); });
  queue_.notify_all();
  if (worker_.joinable())
    worker_.join();
}

/**
 * @brief Run @p work bracketed by on_before_dispatch()/on_after_dispatch().
 *
 * Called from the worker thread. Obtains a stable context& via a brief
 * ctx_ lock released before the hooks or @p work run.
 */
bison::dynamic standalone::dispatch(const std::function<bison::dynamic()>& work) {
  context* ctx_ptr;
  {
    auto lp = ctx_.wlock();
    ctx_ptr = &*lp;
  }
  on_before_dispatch(*ctx_ptr);
  try {
    bison::dynamic result = work();
    on_after_dispatch(*ctx_ptr);
    return result;
  } catch (...) {
    on_after_dispatch(*ctx_ptr);
    throw;
  }
}

// ── Private operation handlers ────────────────────────────────────────────

/**
 * @brief Return metadata for one or all registered classes.
 *
 * Mirrors `server::handle_describe` without transport.
 */
bison::dynamic standalone::handle_describe(bison::key_t ns, bison::key_t klass) {
  bison::dynamic resp;
  auto lp = bison::dynamic::getRegistry().rlock();
  const auto& classes = *lp;

  if (static_cast<bison::hash_t>(klass) == 0u) {
    auto nsIt = classes.find(ns);
    if (nsIt == classes.end())
      return resp;

    std::size_t idx = 0;
    for (const auto& [k, proto] : nsIt->second) {
      if (k == CLASS_ENVELOPE)
        continue;
      bison::dynamic desc;
      desc[FIELD_KLASS] = k;
      // Attach class-level attribute metadata from the CLASS field.
      const auto* class_field = proto->findField(bison::dynamic::CLASS);
      if (class_field)
        apply_attrs(desc, *class_field);
      resp[idx++] = bison::dynamic_ptr{std::move(desc)};
    }
  } else {
    const bison::dynamic* proto = nullptr;
    auto nsIt = classes.find(ns);
    if (nsIt != classes.end()) {
      auto it = nsIt->second.find(klass);
      if (it != nsIt->second.end())
        proto = it->second.get();
    }
    if (!proto)
      throw std::runtime_error("Class not found");

    resp[FIELD_KLASS] = klass;
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

  return resp;
}

/**
 * @brief Create and store a new object instance.
 *
 * Mirrors `server::handle_instantiate` without transport: creates the
 * object via `on_create_object` (default `bison::dynamic::create_instance`,
 * which -- unlike `bison::dynamic::instantiate` -- honors a factory
 * registered via `dynamic::addClass(..., factory)`). Returns the response
 * payload containing the new object ID.
 */
bison::dynamic standalone::handle_instantiate(bison::key_t ns, bison::key_t klass, bison::dynamic params) {
  {
    auto lp = bison::dynamic::getRegistry().rlock();
    auto nsIt = lp->find(ns);
    if (nsIt == lp->end() || !nsIt->second.count(klass))
      throw std::runtime_error(
          std::string("Class not registered in requested namespace: ") +
          std::to_string(static_cast<bison::hash_t>(klass)));
  }

  const bison::key_t oid = shared::generate_id();
  bison::dynamic_ptr obj;
  {
    auto lp = ctx_.wlock();
    obj = on_create_object(*lp, ns, klass);
    lp->objects[oid.id] = obj;
  }

  if (obj->findMethod(HOOK_CONSTRUCT) != nullptr) {
    obj->call(HOOK_CONSTRUCT, params);
  }

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
  auto lp = ctx_.wlock();

  auto it = lp->objects.find(object_id.id);
  if (it == lp->objects.end())
    throw std::runtime_error("Object not found");
  auto& obj = *it->second;

  bison::key_t klass_key = obj.as<bison::key_t>(bison::dynamic::CLASS);
  {
    // Resolve the namespace from the existing object so we look in the
    // correct collection.
    bison::key_t ns{0U};
    auto* nsField = obj.findField(bison::dynamic::NAMESPACE);
    if (nsField && nsField->is<bison::key_t>())
      ns = nsField->as<bison::key_t>();

    auto reg = bison::dynamic::getRegistry().rlock();
    const auto& nsmap = *reg;
    auto nsIt = nsmap.find(ns);
    if (nsIt != nsmap.end()) {
      auto class_it = nsIt->second.find(klass_key);
      if (class_it != nsIt->second.end() && class_it->second)
        obj = class_it->second->clone();
      else
        obj = bison::dynamic::instantiate(ns, klass_key);
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
bison::dynamic standalone::handle_set(bison::key_t object_id, bison::dynamic payload) {
  auto lp = ctx_.wlock();

  auto it = lp->objects.find(object_id.id);
  if (it == lp->objects.end())
    throw std::runtime_error("Object not found");
  auto& obj = *it->second;

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
bison::dynamic standalone::handle_get(bison::key_t object_id, bison::dynamic projection) {
  auto lp = ctx_.wlock();

  auto it = lp->objects.find(object_id.id);
  if (it == lp->objects.end())
    throw std::runtime_error("Object not found");
  auto& obj = *it->second;

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
      result = obj.call(HOOK_GETTER, result);
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
bison::dynamic standalone::handle_call(bison::key_t object_id, bison::dynamic payload, bool oneway) {
  auto lp = ctx_.wlock();

  auto it = lp->objects.find(object_id.id);
  if (it == lp->objects.end())
    throw std::runtime_error("Object not found");
  auto& obj = *it->second;

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
  {
    auto lp = ctx_.wlock();
    auto it = lp->objects.find(object_id.id);
    if (it == lp->objects.end())
      throw std::runtime_error("Object not found");

    if (it->second && it->second->findMethod(HOOK_DESTRUCT) != nullptr) {
      try {
        it->second->call(HOOK_DESTRUCT, bison::dynamic{});
      } catch (...) {
      }
    }

    lp->objects.erase(it);
  }
  unregister_object_events(object_id);
  return bison::dynamic{};
}

/**
 * @brief Return hash→display-name dictionary for all registered classes.
 *
 * Mirrors `server::handle_dictionary` without transport.
 */
bison::dynamic standalone::handle_dictionary() {
  using namespace bison;
  dynamic dict;
  {
    auto lp = dynamic::getRegistry().rlock();
    for (const auto& [ns_key, classes] : *lp) {
      for (const auto& [klass_key, proto] : classes) {
        if (klass_key == CLASS_ENVELOPE)
          continue;
        const auto* class_field = proto->findField(dynamic::CLASS);
        if (class_field) {
          if (auto* dn = class_field->findAttribute<DisplayName>())
            dict[klass_key] = dn->name();
        }
        proto->forEach([&](key_t k, const field& f) {
          if (k == dynamic::CLASS || k == dynamic::PARENT || k == dynamic::NAMESPACE)
            return;
          if (auto* dn = f.findAttribute<DisplayName>())
            dict[k] = dn->name();
        });
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
  return dict;
}

/**
 * @brief Return human-readable help text for all registered classes.
 *
 * Mirrors `server::handle_help` without transport.
 */
bison::dynamic standalone::handle_help() {
  using namespace bison;
  std::ostringstream oss;
  oss << "Registered classes:\n";

  {
    auto lp = dynamic::getRegistry().rlock();
    for (const auto& [ns_key, classes] : *lp) {
      for (const auto& [klass_key, proto] : classes) {
        if (klass_key == CLASS_ENVELOPE)
          continue;
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
  return resp;
}

} // namespace bdg::bison::rmi
