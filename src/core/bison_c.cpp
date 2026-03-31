// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file bison_c.cpp
 * @brief C++ implementation of the pure-C Bison shared-library API.
 *
 * Each exported function wraps one or more calls into the C++ bison library.
 * All C++ exceptions are caught at the boundary and converted to `bison_error`
 * return codes so that no exception can propagate through the C ABI.
 *
 * ### Handle representation
 * A `bison_handle` is a pointer to a heap-allocated
 * `std::shared_ptr<bdg::bison::dynamic>`.  This keeps the ref-counting
 * transparent: `bison_add_ref` allocates a *new* `shared_ptr` that shares
 * ownership with the original; `bison_release` deletes the `shared_ptr`,
 * decrementing the underlying object's ref-count (and destroying it when it
 * reaches zero).
 *
 * Non-owning handles returned by `bison_find_class` are raw pointers wrapped
 * in a sentinel struct so callers cannot accidentally call `bison_release` on
 * them (doing so on a non-owning handle would be a double-free).  In practice
 * the C API documents this contract clearly and the type is the same opaque
 * `bison_handle`; callers must follow the documented ownership rules.
 */

#include "bison_c.h"
#include "bison.hpp"

#include <cstring>
#include <memory>
#include <stdexcept>

// ─── Internal helpers ──────────────────────────────────────────────────────

// Each *owning* handle is a heap-allocated shared_ptr<dynamic>.
using sp_dyn = std::shared_ptr<bdg::bison::dynamic>;

/** Cast an opaque bison_handle back to a shared_ptr<dynamic>*. */
static inline sp_dyn* as_sp(bison_handle h) {
  return reinterpret_cast<sp_dyn*>(h);
}

/** Cast a shared_ptr<dynamic>* to the opaque handle type. */
static inline bison_handle as_handle(sp_dyn* p) {
  return reinterpret_cast<bison_handle>(p);
}

/** Dereference a handle to the underlying dynamic object.  Returns nullptr if h
 * is null or the shared_ptr is empty. */
static inline bdg::bison::dynamic* dyn(bison_handle h) {
  if (!h)
    return nullptr;
  return as_sp(h)->get();
}

// ─── Lifecycle ─────────────────────────────────────────────────────────────

BISON_API bison_handle bison_create(bison_hash klass_name) {
  try {
    auto* sp = new sp_dyn(
        std::make_shared<bdg::bison::dynamic>(bdg::bison::key_t{klass_name}));
    return as_handle(sp);
  } catch (...) {
    return nullptr;
  }
}

BISON_API bison_handle bison_instantiate(bison_hash klass_name) {
  try {
    bdg::bison::dynamic obj =
        bdg::bison::dynamic::instantiate(bdg::bison::key_t{klass_name});
    auto* sp =
        new sp_dyn(std::make_shared<bdg::bison::dynamic>(std::move(obj)));
    return as_handle(sp);
  } catch (...) {
    return nullptr;
  }
}

BISON_API bison_handle bison_add_ref(bison_handle h) {
  if (!h)
    return nullptr;
  try {
    auto* original = as_sp(h);
    auto* copy = new sp_dyn(*original); // increments shared_ptr refcount
    return as_handle(copy);
  } catch (...) {
    return nullptr;
  }
}

BISON_API void bison_release(bison_handle h) {
  if (!h)
    return;
  delete as_sp(h); // decrements shared_ptr refcount; may destroy the object
}

// ─── Import helpers ─────────────────────────────────────────────────────────

BISON_API bison_handle bison_from_json(const char* json) {
  if (!json)
    return nullptr;
  try {
    auto sp = bdg::bison::extensions::from_json(json);
    return as_handle(new sp_dyn(std::move(sp)));
  } catch (...) {
    return nullptr;
  }
}

BISON_API bison_handle bison_from_yaml(const char* yaml) {
  if (!yaml)
    return nullptr;
  try {
    auto sp = bdg::bison::extensions::from_yaml(yaml);
    return as_handle(new sp_dyn(std::move(sp)));
  } catch (...) {
    return nullptr;
  }
}

// ─── Class registry ─────────────────────────────────────────────────────────

BISON_API bison_error
bison_add_class(bison_hash parent_name, bison_handle klass) {
  if (!klass)
    return BISON_ERR_NULL;
  try {
    // addClass takes a shared_ptr; copy the one inside the handle.
    sp_dyn copy = *as_sp(klass);
    bool ok = bdg::bison::dynamic::addClass(
        bdg::bison::key_t{parent_name}, std::move(copy));
    return ok ? BISON_OK : BISON_ERR_DUPLICATE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_handle bison_find_class(bison_handle h, bison_hash name) {
  if (!h)
    return nullptr;
  try {
    bdg::bison::dynamic* found = dyn(h)->findClass(bdg::bison::key_t{name});
    if (!found)
      return nullptr;
    // Return a new owning handle that shares ownership via a separate
    // shared_ptr constructed from the raw pointer and the existing shared_ptr.
    // We need to get the shared_ptr from the registry to do this properly.
    // Use findClass result with a no-op deleter as a non-owning view.
    // Since the class registry holds a shared_ptr, we can look it up:
    auto lp = bdg::bison::dynamic::getRegistry().rlock();
    auto& classes = *lp;
    auto key = found->at(bdg::bison::dynamic::CLASS).as<bdg::bison::key_t>();
    auto it = classes.find(key);
    if (it == classes.end())
      return nullptr;
    auto* sp = new sp_dyn(it->second); // owning copy from registry
    return as_handle(sp);
  } catch (...) {
    return nullptr;
  }
}

// ─── Setters ────────────────────────────────────────────────────────────────

BISON_API bison_error
bison_set_int(bison_handle h, bison_hash name, int32_t value) {
  if (!h)
    return BISON_ERR_NULL;
  try {
    (*dyn(h))[bdg::bison::key_t{name}] = value;
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error
bison_set_float(bison_handle h, bison_hash name, float value) {
  if (!h)
    return BISON_ERR_NULL;
  try {
    (*dyn(h))[bdg::bison::key_t{name}] = value;
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error
bison_set_bool(bison_handle h, bison_hash name, int value) {
  if (!h)
    return BISON_ERR_NULL;
  try {
    (*dyn(h))[bdg::bison::key_t{name}] = bool(value != 0);
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error
bison_set_string(bison_handle h, bison_hash name, const char* value) {
  if (!h || !value)
    return BISON_ERR_NULL;
  try {
    (*dyn(h))[bdg::bison::key_t{name}] = std::string(value);
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error
bison_set_object(bison_handle h, bison_hash name, bison_handle value) {
  if (!h)
    return BISON_ERR_NULL;
  try {
    // value == nullptr means set a null dynamic ref.
    sp_dyn child = value ? *as_sp(value) : sp_dyn{};
    (*dyn(h))[bdg::bison::key_t{name}] = child;
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error
bison_set_int_at(bison_handle h, size_t index, int32_t value) {
  if (!h)
    return BISON_ERR_NULL;
  try {
    (*dyn(h))[index] = value;
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error
bison_set_float_at(bison_handle h, size_t index, float value) {
  if (!h)
    return BISON_ERR_NULL;
  try {
    (*dyn(h))[index] = value;
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error
bison_set_string_at(bison_handle h, size_t index, const char* value) {
  if (!h || !value)
    return BISON_ERR_NULL;
  try {
    (*dyn(h))[index] = std::string(value);
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

// ─── Getters ────────────────────────────────────────────────────────────────

BISON_API bison_error
bison_get_int(bison_handle h, bison_hash name, int32_t* out) {
  if (!h || !out)
    return BISON_ERR_NULL;
  try {
    *out = (*dyn(h))[bdg::bison::key_t{name}].as<int32_t>();
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error
bison_get_float(bison_handle h, bison_hash name, float* out) {
  if (!h || !out)
    return BISON_ERR_NULL;
  try {
    *out = (*dyn(h))[bdg::bison::key_t{name}].as<float>();
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error
bison_get_bool(bison_handle h, bison_hash name, int* out) {
  if (!h || !out)
    return BISON_ERR_NULL;
  try {
    *out = (*dyn(h))[bdg::bison::key_t{name}].as<bool>() ? 1 : 0;
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_get_string(
    bison_handle h,
    bison_hash name,
    char* buf,
    size_t buf_len,
    size_t* len_out) {
  if (!h)
    return BISON_ERR_NULL;
  try {
    const std::string& s = (*dyn(h))[bdg::bison::key_t{name}].as<std::string>();
    if (len_out)
      *len_out = s.size();
    if (buf && buf_len > 0) {
      size_t copy_len = s.size() < buf_len - 1 ? s.size() : buf_len - 1;
      std::memcpy(buf, s.data(), copy_len);
      buf[copy_len] = '\0';
    }
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error
bison_get_object(bison_handle h, bison_hash name, bison_handle* out) {
  if (!h || !out)
    return BISON_ERR_NULL;
  try {
    auto sp = (*dyn(h))[bdg::bison::key_t{name}].as<bdg::bison::dynamic_ptr>();
    if (!sp) {
      *out = nullptr;
      return BISON_OK;
    }
    *out = as_handle(new sp_dyn(sp));
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error
bison_get_int_at(bison_handle h, size_t index, int32_t* out) {
  if (!h || !out)
    return BISON_ERR_NULL;
  try {
    *out = (*dyn(h))[index].as<int32_t>();
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error
bison_get_float_at(bison_handle h, size_t index, float* out) {
  if (!h || !out)
    return BISON_ERR_NULL;
  try {
    *out = (*dyn(h))[index].as<float>();
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_get_string_at(
    bison_handle h,
    size_t index,
    char* buf,
    size_t buf_len,
    size_t* len_out) {
  if (!h)
    return BISON_ERR_NULL;
  try {
    const std::string& s = (*dyn(h))[index].as<std::string>();
    if (len_out)
      *len_out = s.size();
    if (buf && buf_len > 0) {
      size_t copy_len = s.size() < buf_len - 1 ? s.size() : buf_len - 1;
      std::memcpy(buf, s.data(), copy_len);
      buf[copy_len] = '\0';
    }
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API size_t bison_size(bison_handle h) {
  if (!h)
    return 0;
  try {
    return dyn(h)->size();
  } catch (...) {
    return 0;
  }
}

// ─── Methods ────────────────────────────────────────────────────────────────

BISON_API bison_error bison_add_method(
    bison_handle h,
    bison_hash name,
    bison_method_fn fn,
    void* user) {
  if (!h || !fn)
    return BISON_ERR_NULL;
  try {
    bdg::bison::method wrapped =
        [fn, user](
            bdg::bison::dynamic& self,
            const bdg::bison::dynamic& params) -> bdg::bison::dynamic {
      // Create a non-owning handle for self using a no-op deleter.
      // We wrap &self (a C++ reference to the actual dynamic) in a
      // shared_ptr that does NOT delete the object when destroyed, so
      // the C callback can read and mutate self in-place through the
      // handle without any extra copy/propagation step.
      auto* self_sp = new sp_dyn(&self, [](bdg::bison::dynamic*) {});
      // params is const; make a heap copy so the callback can hold a handle.
      auto* param_sp =
          new sp_dyn(std::make_shared<bdg::bison::dynamic>(params));
      // result is a fresh empty dynamic; caller populates it.
      auto result_dyn = std::make_shared<bdg::bison::dynamic>();
      auto* res_sp = new sp_dyn(result_dyn);

      fn(as_handle(self_sp), as_handle(param_sp), as_handle(res_sp), user);

      bdg::bison::dynamic result = std::move(*result_dyn);

      delete self_sp;
      delete param_sp;
      delete res_sp;

      return result;
    };

    bool ok = dyn(h)->addMethod(bdg::bison::key_t{name}, wrapped);
    return ok ? BISON_OK : BISON_ERR_DUPLICATE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_call(
    bison_handle h,
    bison_hash name,
    bison_handle params,
    bison_handle* result) {
  if (!h || !params || !result)
    return BISON_ERR_NULL;
  try {
    bdg::bison::dynamic ret =
        dyn(h)->call(bdg::bison::key_t{name}, *dyn(params));
    *result = as_handle(
        new sp_dyn(std::make_shared<bdg::bison::dynamic>(std::move(ret))));
    return BISON_OK;
  } catch (const std::runtime_error& e) {
    std::string msg(e.what());
    if (msg.find("not found") != std::string::npos ||
        msg.find("Not found") != std::string::npos)
      return BISON_ERR_NOT_FOUND;
    return BISON_ERR_EXCEPTION;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

// ─── Utility ────────────────────────────────────────────────────────────────

BISON_API bison_hash bison_key(const char* name) {
  if (!name)
    return 0;
  return bdg::bison::hash(name);
}

// ─── RMI – Remote Method Invocation ─────────────────────────────────────────

#include "src/rmi/rmi.hpp"

// Convenience alias
namespace rmi = bdg::bison::rmi;
namespace rmi_t = bdg::bison::rmi::transport;

// ── Internal handle structs ───────────────────────────────────────────────

struct bison_rmi_transport_ {
  rmi_t::memory_server_transport transport;
};

struct bison_rmi_server_ {
  // The server holds a reference to the transport; the transport_ pointer
  // is borrowed (owned by the bison_rmi_transport_ handle).
  rmi_t::memory_server_transport* transport_ptr;
  rmi::server* srv;

  bison_rmi_server_(rmi_t::memory_server_transport* t)
      : transport_ptr(t), srv(new rmi::server(*t)) {}

  ~bison_rmi_server_() {
    delete srv;
  }
};

struct bison_rmi_client_ {
  rmi::client* c;
  explicit bison_rmi_client_(rmi::client* client) : c(client) {}
  ~bison_rmi_client_() {
    delete c;
  }
};

struct bison_rmi_proxy_ {
  rmi::proxy::dynamic proxy;
  explicit bison_rmi_proxy_(rmi::proxy::dynamic&& p) : proxy(std::move(p)) {}
};

// ── Transport ─────────────────────────────────────────────────────────────

BISON_API bison_rmi_transport bison_rmi_transport_create(void) {
  try {
    return new bison_rmi_transport_();
  } catch (...) {
    return nullptr;
  }
}

BISON_API void bison_rmi_transport_destroy(bison_rmi_transport t) {
  delete t;
}

// ── Server ────────────────────────────────────────────────────────────────

BISON_API bison_rmi_server bison_rmi_server_create(bison_rmi_transport t) {
  if (!t)
    return nullptr;
  try {
    return new bison_rmi_server_(&t->transport);
  } catch (...) {
    return nullptr;
  }
}

BISON_API bison_error bison_rmi_server_listen(bison_rmi_server srv) {
  if (!srv)
    return BISON_ERR_NULL;
  try {
    srv->srv->listen();
    return BISON_OK;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API void bison_rmi_server_stop(bison_rmi_server srv) {
  if (!srv)
    return;
  try {
    srv->srv->stop();
  } catch (...) {
  }
}

BISON_API void bison_rmi_server_destroy(bison_rmi_server srv) {
  delete srv;
}

// ── Client ────────────────────────────────────────────────────────────────

BISON_API bison_rmi_client bison_rmi_client_create(bison_rmi_transport t) {
  if (!t)
    return nullptr;
  try {
    // connect() returns a memory_client_transport by value
    auto client_transport = t->transport.connect();
    auto* c = new rmi::client(std::move(client_transport));
    return new bison_rmi_client_(c);
  } catch (...) {
    return nullptr;
  }
}

BISON_API bison_error bison_rmi_client_connect(bison_rmi_client c) {
  if (!c)
    return BISON_ERR_NULL;
  try {
    c->c->connect();
    return BISON_OK;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_rmi_client_disconnect(bison_rmi_client c) {
  if (!c)
    return BISON_ERR_NULL;
  try {
    c->c->disconnect();
    return BISON_OK;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_handle
bison_rmi_client_describe(bison_rmi_client c, bison_hash klass) {
  if (!c)
    return nullptr;
  try {
    auto fut = c->c->describe(bdg::bison::key_t{klass});
    bdg::bison::dynamic result = fut.get();
    return as_handle(
        new sp_dyn(std::make_shared<bdg::bison::dynamic>(std::move(result))));
  } catch (...) {
    return nullptr;
  }
}

BISON_API bison_rmi_proxy bison_rmi_client_instantiate(
    bison_rmi_client c,
    bison_hash klass,
    bison_handle params) {
  if (!c)
    return nullptr;
  try {
    bdg::bison::dynamic p;
    if (params && dyn(params))
      p = dyn(params)->clone();

    auto fut = c->c->instantiate(bdg::bison::key_t{klass}, std::move(p));
    auto proxy = fut.get();
    return new bison_rmi_proxy_(std::move(proxy));
  } catch (...) {
    return nullptr;
  }
}

BISON_API bison_error
bison_rmi_client_destroy_proxy(bison_rmi_client c, bison_rmi_proxy proxy) {
  if (!c || !proxy)
    return BISON_ERR_NULL;
  try {
    c->c->destroy(std::move(proxy->proxy));
    delete proxy;
    return BISON_OK;
  } catch (...) {
    delete proxy;
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API void bison_rmi_client_destroy(bison_rmi_client c) {
  delete c;
}

// ── Remote proxy operations ───────────────────────────────────────────────

BISON_API bison_error bison_rmi_proxy_clear(bison_rmi_proxy proxy) {
  if (!proxy)
    return BISON_ERR_NULL;
  try {
    proxy->proxy.clear();
    return BISON_OK;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error
bison_rmi_proxy_set(bison_rmi_proxy proxy, bison_handle fields) {
  if (!proxy || !fields)
    return BISON_ERR_NULL;
  try {
    proxy->proxy.set(*dyn(fields));
    return BISON_OK;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error
bison_rmi_proxy_get(bison_rmi_proxy proxy, bison_handle* fields_out) {
  if (!proxy || !fields_out)
    return BISON_ERR_NULL;
  try {
    // If *fields_out is non-null, use it as a projection; otherwise full get.
    bdg::bison::dynamic projection;
    if (*fields_out && dyn(*fields_out))
      projection = dyn(*fields_out)->clone();

    proxy->proxy.get(std::move(projection));

    // Release old handle and return new result.
    if (*fields_out) {
      bison_release(*fields_out);
      *fields_out = nullptr;
    }
    *fields_out = as_handle(new sp_dyn(
        std::make_shared<bdg::bison::dynamic>(std::move(projection))));
    return BISON_OK;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_rmi_proxy_call(
    bison_rmi_proxy proxy,
    bison_hash method_name,
    bison_handle params,
    bison_handle* result_out) {
  if (!proxy || !params)
    return BISON_ERR_NULL;
  try {
    bdg::bison::key_t name{static_cast<bdg::bison::hash_t>(method_name)};
    auto fut = proxy->proxy.call(name, std::move(*dyn(params)));
    auto result = fut.get();
    if (result_out) {
      *result_out = as_handle(
          new sp_dyn(std::make_shared<bdg::bison::dynamic>(std::move(result))));
    }
    return BISON_OK;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_rmi_proxy_call_oneway(
    bison_rmi_proxy proxy,
    bison_hash method_name,
    bison_handle params) {
  if (!proxy || !params)
    return BISON_ERR_NULL;
  try {
    bdg::bison::key_t name{static_cast<bdg::bison::hash_t>(method_name)};
    proxy->proxy.call(name, std::move(*dyn(params)), /*oneway=*/true);
    return BISON_OK;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_rmi_proxy_on_event(
    bison_rmi_proxy proxy,
    bison_hash event_name,
    bison_rmi_event_fn handler,
    void* user) {
  if (!proxy || !handler)
    return BISON_ERR_NULL;
  try {
    proxy->proxy.onEvent(
        bdg::bison::key_t{event_name},
        [handler, user](bdg::bison::dynamic params) {
          // Wrap params in a temporary non-owning handle for the callback.
          // The handle is stack-allocated and must NOT be released by the
          // caller.
          auto sp = std::make_shared<bdg::bison::dynamic>(std::move(params));
          sp_dyn holder(sp);
          bison_handle tmp_h = as_handle(&holder);
          handler(tmp_h, user);
          // holder goes out of scope here; the underlying dynamic is released.
        });
    return BISON_OK;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}
