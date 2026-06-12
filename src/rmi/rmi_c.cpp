// MIT License © 2025 Binary Dice Games
/**
 * @file rmi_c.cpp
 * @brief C++ implementation of the pure-C RMI shared-library API.
 *
 * Each exported function wraps one or more calls into the C++ RMI library.
 * All C++ exceptions are caught at the boundary and converted to `rmi_error`
 * return codes so that no exception can propagate through the C ABI.
 */

#include "../../include/rmi_c.h"
#include "rmi.hpp"

#include <cstring>
#include <memory>
#include <stdexcept>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;

// ─── Internal helpers ──────────────────────────────────────────────────────

/**
 * @brief Unified client handle state supporting both transport-backed clients
 *        and standalone in-process sessions.
 *
 * Exactly one of `client_owner`, `standalone_owner`, or `borrowed_client` is
 * active at a time:
 * - `client_owner` — owns a transport-backed `client` created by
 *   `rmi_client_tcp_create`.
 * - `standalone_owner` — owns a `standalone` instance created by
 *   `rmi_standalone_create`.
 * - `borrowed_client` — non-owning pointer to a `client` supplied by a
 *   callback context (PTY client app).
 */
struct client_state {
  std::unique_ptr<client> client_owner;
  std::unique_ptr<standalone> standalone_owner;
  client* borrowed_client = nullptr;
  bool owns_resource =
      true; /**< True when this state owns the underlying object. */
  bool released = false;

  /** @brief True when this state wraps a standalone session. */
  bool is_standalone() const {
    return standalone_owner != nullptr;
  }

  /** @brief True when this handle is valid and not yet released. */
  bool is_valid() const {
    if (released)
      return false;
    if (is_standalone())
      return standalone_owner != nullptr;
    return (owns_resource ? client_owner.get() : borrowed_client) != nullptr;
  }
};

/** Cast to client_state* */
static inline client_state* as_client_state(rmi_client_handle h) {
  return reinterpret_cast<client_state*>(h);
}

/** Cast from client_state* */
static inline rmi_client_handle as_client_handle(client_state* p) {
  return reinterpret_cast<rmi_client_handle>(p);
}

/** Create an owning transport-backed client handle. */
static inline rmi_client_handle make_owned_client_handle(
    std::unique_ptr<client>&& owned_client) {
  auto* state = new client_state{};
  state->client_owner = std::move(owned_client);
  state->owns_resource = true;
  state->released = false;
  return as_client_handle(state);
}

/** Create a non-owning callback-scoped client handle. */
static inline rmi_client_handle make_borrowed_client_handle(client& borrowed) {
  auto* state = new client_state{};
  state->borrowed_client = &borrowed;
  state->owns_resource = false;
  state->released = false;
  return as_client_handle(state);
}

/** Create an owning standalone handle. */
static inline rmi_client_handle make_owned_standalone_handle(
    std::unique_ptr<standalone>&& sa) {
  auto* state = new client_state{};
  state->standalone_owner = std::move(sa);
  state->owns_resource = true;
  state->released = false;
  return as_client_handle(state);
}

/**
 * @brief Dereference a transport-backed client safely.
 *
 * Returns nullptr if @p h is null, released, or wraps a standalone session.
 * Proxy operations that only need a validity check should use
 * `client_handle_is_valid()` instead.
 */
static inline client* client_deref(rmi_client_handle h) {
  if (!h)
    return nullptr;
  client_state* state = as_client_state(h);
  if (!state || state->released || state->is_standalone())
    return nullptr;
  return state->owns_resource ? state->client_owner.get()
                              : state->borrowed_client;
}

/**
 * @brief Dereference a standalone session safely.
 *
 * Returns nullptr if @p h is null, released, or wraps a transport-backed
 * client.
 */
static inline standalone* standalone_deref(rmi_client_handle h) {
  if (!h)
    return nullptr;
  client_state* state = as_client_state(h);
  if (!state || state->released || !state->is_standalone())
    return nullptr;
  return state->standalone_owner.get();
}

/**
 * @brief Return true when @p h is a valid, unreleased client handle.
 *
 * Accepts both transport-backed and standalone handles.  Use this for proxy
 * operations that need only a validity check rather than an actual `client*`.
 */
static inline bool client_handle_is_valid(rmi_client_handle h) {
  if (!h)
    return false;
  return as_client_state(h)->is_valid();
}

// For server: wrapper around std::unique_ptr<server>
using server_ptr = std::unique_ptr<server>;

/** Cast to server_ptr* */
static inline server_ptr* as_server_ptr(rmi_server_handle h) {
  return reinterpret_cast<server_ptr*>(h);
}

/** Cast from server_ptr* */
static inline rmi_server_handle as_server_handle(server_ptr* p) {
  return reinterpret_cast<rmi_server_handle>(p);
}

/** Dereference safely */
static inline server* server_deref(rmi_server_handle h) {
  if (!h)
    return nullptr;
  return as_server_ptr(h)->get();
}

// For proxy: wrapper around std::unique_ptr<proxy::dynamic>
using proxy_ptr = std::unique_ptr<proxy::dynamic>;

/** Cast to proxy_ptr* */
static inline proxy_ptr* as_proxy_ptr(rmi_proxy_handle h) {
  return reinterpret_cast<proxy_ptr*>(h);
}

/** Cast from proxy_ptr* */
static inline rmi_proxy_handle as_proxy_handle(proxy_ptr* p) {
  return reinterpret_cast<rmi_proxy_handle>(p);
}

/** Dereference safely */
static inline proxy::dynamic* proxy_deref(rmi_proxy_handle h) {
  if (!h)
    return nullptr;
  return as_proxy_ptr(h)->get();
}

// For bison value: wrapper around heap-allocated std::shared_ptr<dynamic>
using bison_dynamic_ptr = std::shared_ptr<dynamic>;

struct future_state_base;

/** Cast to bison_dynamic_ptr* */
static inline bison_dynamic_ptr* as_dynamic_ptr(bison_handle h) {
  return reinterpret_cast<bison_dynamic_ptr*>(h);
}

/** Cast from bison_dynamic_ptr* */
static inline bison_handle as_bison_handle(bison_dynamic_ptr* p) {
  return reinterpret_cast<bison_handle>(p);
}

/** Cast to future_state_base* */
static inline future_state_base* as_future_state(rmi_future_handle h) {
  return reinterpret_cast<future_state_base*>(h);
}

/** Cast from future_state_base* */
static inline rmi_future_handle as_future_handle(future_state_base* p) {
  return reinterpret_cast<rmi_future_handle>(p);
}

/** Wrap a dynamic value into a new heap-owned bison_handle */
static inline bison_handle dynamic_to_bison_handle(dynamic val) {
  auto* p = new bison_dynamic_ptr(std::make_shared<dynamic>(std::move(val)));
  return as_bison_handle(p);
}

/** Extract a dynamic value from a bison_handle; returns empty dynamic if null
 */
static inline dynamic bison_handle_to_dynamic(bison_handle h) {
  if (!h)
    return dynamic{};
  bison_dynamic_ptr* p = as_dynamic_ptr(h);
  if (!*p)
    return dynamic{};
  return dynamic(**p);
}

static inline rmi_error map_runtime_error(const std::runtime_error& e) {
  std::string msg = e.what();
  if (msg.find("remote") != std::string::npos ||
      msg.find("server") != std::string::npos)
    return RMI_ERR_REMOTE_EXCEPTION;
  return RMI_ERR_INVALID_STATE;
}

static inline rmi_error map_app_exit_code(int exit_code) {
  return exit_code == 0 ? RMI_OK : RMI_ERR_INVALID_STATE;
}

class c_pty_client_application_with_callbacks final
    : public apps::pty_client_application {
 public:
  explicit c_pty_client_application_with_callbacks(
      const rmi_pty_client_callbacks* callbacks)
      : callbacks_(callbacks) {}

 protected:
  int on_session(client& rmi_client, const run_context&) override {
    rmi_client_handle callback_client = make_borrowed_client_handle(rmi_client);
    int result = 1;
    try {
      result = callbacks_->on_session(callback_client, callbacks_->user);
    } catch (...) {
      result = 1;
    }

    delete as_client_state(callback_client);
    return result;
  }

  void on_error(const std::string& message) const override {
    if (callbacks_ && callbacks_->on_error) {
      callbacks_->on_error(message.c_str(), callbacks_->user);
      return;
    }
    apps::pty_client_application::on_error(message);
  }

 private:
  const rmi_pty_client_callbacks* callbacks_ = nullptr;
};

class c_pty_server_application_with_callbacks final
    : public apps::pty_server_application {
 public:
  explicit c_pty_server_application_with_callbacks(
      const rmi_pty_server_callbacks* callbacks)
      : callbacks_(callbacks) {}

 protected:
  void register_classes() override {
    callbacks_->register_classes(callbacks_->user);
  }

  void on_error(const std::string& message) const override {
    if (callbacks_ && callbacks_->on_error) {
      callbacks_->on_error(message.c_str(), callbacks_->user);
      return;
    }
    apps::pty_server_application::on_error(message);
  }

 private:
  const rmi_pty_server_callbacks* callbacks_ = nullptr;
};

template <typename T>
static inline rmi_error wait_future_ready(
    std::future<T>& fut,
    int64_t timeout_ms) {
  try {
    std::chrono::milliseconds wait_time{timeout_ms < 0 ? 5000 : timeout_ms};
    auto wait_status = fut.wait_for(wait_time);

    if (wait_status == std::future_status::timeout)
      return RMI_ERR_TIMEOUT;
    return RMI_OK;
  } catch (const std::future_error&) {
    return RMI_ERR_INVALID_STATE;
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

struct future_state_base {
  virtual ~future_state_base() = default;
  virtual rmi_error wait(int64_t timeout_ms) = 0;
  virtual rmi_error take_dynamic(bison_handle* out_value) {
    (void)out_value;
    return RMI_ERR_INVALID_STATE;
  }
  virtual rmi_error take_proxy(rmi_proxy_handle* out_proxy) {
    (void)out_proxy;
    return RMI_ERR_INVALID_STATE;
  }
};

struct bool_future_state final : future_state_base {
  explicit bool_future_state(std::future<bool>&& future)
      : future_(std::move(future)) {}

  rmi_error wait(int64_t timeout_ms) override {
    return wait_future_ready(future_, timeout_ms);
  }

  std::future<bool> future_;
};

struct dynamic_future_state final : future_state_base {
  explicit dynamic_future_state(std::future<dynamic>&& future)
      : future_(std::move(future)) {}

  rmi_error wait(int64_t timeout_ms) override {
    return wait_future_ready(future_, timeout_ms);
  }

  rmi_error take_dynamic(bison_handle* out_value) override {
    if (!out_value)
      return RMI_ERR_NULL;
    try {
      *out_value = dynamic_to_bison_handle(future_.get());
      return RMI_OK;
    } catch (const std::runtime_error& e) {
      return map_runtime_error(e);
    } catch (const std::future_error&) {
      return RMI_ERR_INVALID_STATE;
    } catch (...) {
      return RMI_ERR_EXCEPTION;
    }
  }

  std::future<dynamic> future_;
};

struct proxy_future_state final : future_state_base {
  explicit proxy_future_state(std::future<proxy::dynamic>&& future)
      : future_(std::move(future)) {}

  rmi_error wait(int64_t timeout_ms) override {
    return wait_future_ready(future_, timeout_ms);
  }

  rmi_error take_proxy(rmi_proxy_handle* out_proxy) override {
    if (!out_proxy)
      return RMI_ERR_NULL;
    try {
      auto proxy = future_.get();
      auto* pp =
          new proxy_ptr(std::make_unique<proxy::dynamic>(std::move(proxy)));
      *out_proxy = as_proxy_handle(pp);
      return RMI_OK;
    } catch (const std::runtime_error& e) {
      return map_runtime_error(e);
    } catch (const std::future_error&) {
      return RMI_ERR_INVALID_STATE;
    } catch (...) {
      return RMI_ERR_EXCEPTION;
    }
  }

  std::future<proxy::dynamic> future_;
};

template <typename TState, typename TFuture>
static inline rmi_error store_future_handle(
    rmi_future_handle* out_future,
    TFuture&& future) {
  if (!out_future)
    return RMI_ERR_NULL;
  try {
    *out_future = as_future_handle(new TState(std::move(future)));
    return RMI_OK;
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

template <typename TConsume>
static inline rmi_error consume_future_handle(
    rmi_future_handle* future,
    TConsume&& consume) {
  if (!future || !*future)
    return RMI_ERR_NULL;

  std::unique_ptr<future_state_base> owned(as_future_state(*future));
  *future = nullptr;
  return consume(*owned);
}

template <typename T>
static inline rmi_error
wait_future_result(std::future<T>& fut, int64_t timeout_ms, T* out = nullptr) {
  rmi_error err = wait_future_ready(fut, timeout_ms);
  if (err != RMI_OK)
    return err;

  T result = fut.get();
  if (out)
    *out = std::move(result);
  return RMI_OK;
}

static inline rmi_error wait_future_bool(
    std::future<bool>& fut,
    int64_t timeout_ms) {
  rmi_error err = wait_future_ready(fut, timeout_ms);
  if (err != RMI_OK)
    return err;

  fut.get();
  return RMI_OK;
}

static inline std::future<dynamic> make_describe_future(
    rmi_client_handle h,
    uint32_t klass) {
  if (auto* sa = standalone_deref(h))
    return sa->describe(bdg::bison::key_t{klass});
  return client_deref(h)->describe(bdg::bison::key_t{klass});
}

static inline std::future<proxy::dynamic> make_instantiate_future(
    rmi_client_handle h,
    uint32_t klass,
    bison_handle params) {
  dynamic dyn_params = bison_handle_to_dynamic(params);
  if (auto* sa = standalone_deref(h))
    return sa->instantiate(bdg::bison::key_t{klass}, std::move(dyn_params));

  client* c = client_deref(h);
  return c->instantiate(0U, bdg::bison::key_t{klass}, std::move(dyn_params));
}

static inline std::future<bool> make_proxy_clear_future(proxy::dynamic* px) {
  return px->clear();
}

static inline std::future<bool> make_proxy_set_future(
    proxy::dynamic* px,
    bison_handle fields) {
  dynamic patch = bison_handle_to_dynamic(fields);
  return px->set(std::move(patch));
}

static inline std::future<dynamic> make_proxy_get_future(
    proxy::dynamic* px,
    bison_handle projection) {
  dynamic query = bison_handle_to_dynamic(projection);
  return projection ? px->get(std::move(query)) : px->get();
}

static inline std::future<dynamic> make_proxy_call_future(
    proxy::dynamic* px,
    uint32_t method,
    bison_handle params) {
  dynamic dyn_params = bison_handle_to_dynamic(params);
  return px->call(bdg::bison::key_t{method}, std::move(dyn_params));
}

// ─── Async futures ────────────────────────────────────────────────────────

RMI_API rmi_error
rmi_future_wait(rmi_future_handle future, int64_t timeout_ms) {
  if (!future)
    return RMI_ERR_NULL;
  return as_future_state(future)->wait(timeout_ms);
}

RMI_API rmi_error
rmi_future_get_dynamic(rmi_future_handle* future, bison_handle* out_value) {
  return consume_future_handle(future, [out_value](future_state_base& state) {
    return state.take_dynamic(out_value);
  });
}

RMI_API rmi_error
rmi_future_get_proxy(rmi_future_handle* future, rmi_proxy_handle* out_proxy) {
  return consume_future_handle(future, [out_proxy](future_state_base& state) {
    return state.take_proxy(out_proxy);
  });
}

RMI_API void rmi_future_release(rmi_future_handle future) {
  if (!future)
    return;
  delete as_future_state(future);
}

// ─── Client ────────────────────────────────────────────────────────────────

RMI_API rmi_client_handle
rmi_client_tcp_create(const char* host, uint16_t port) {
  if (!host)
    return nullptr;
  try {
    return make_owned_client_handle(
        std::make_unique<client>(
            std::make_unique<socket_client_transport>(host, port)));
  } catch (...) {
    return nullptr;
  }
}

RMI_API rmi_client_handle rmi_client_stdio_create(void) {
  try {
    return make_owned_client_handle(
        std::make_unique<client>(std::make_unique<stdio_client_transport>()));
  } catch (...) {
    return nullptr;
  }
}

RMI_API rmi_client_handle rmi_standalone_create(void) {
  try {
    return make_owned_standalone_handle(std::make_unique<standalone>());
  } catch (...) {
    return nullptr;
  }
}

RMI_API rmi_error rmi_client_connect(rmi_client_handle h, bison_handle params) {
  if (!client_handle_is_valid(h))
    return RMI_ERR_NULL;
  // Standalone sessions are always "connected"; skip transport setup.
  if (standalone_deref(h))
    return RMI_OK;
  client* c = client_deref(h);
  if (!c)
    return RMI_ERR_NULL;
  try {
    // For now, ignore the params argument and use default config.
    // Params could be extended in the future if needed.
    (void)params;
    c->connect(dynamic{});
    return RMI_OK;
  } catch (const std::runtime_error&) {
    return RMI_ERR_TRANSPORT;
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API rmi_error rmi_client_describe(
    rmi_client_handle h,
    uint32_t klass,
    bison_handle* out_desc) {
  if (!out_desc)
    return RMI_ERR_NULL;
  if (!client_handle_is_valid(h))
    return RMI_ERR_NULL;
  try {
    dynamic desc = make_describe_future(h, klass).get();
    *out_desc = dynamic_to_bison_handle(std::move(desc));
    return RMI_OK;
  } catch (const std::runtime_error&) {
    return RMI_ERR_INVALID_STATE;
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API rmi_error rmi_client_describe_async(
    rmi_client_handle h,
    uint32_t klass,
    rmi_future_handle* out_future) {
  if (!client_handle_is_valid(h))
    return RMI_ERR_NULL;
  try {
    return store_future_handle<dynamic_future_state>(
        out_future, make_describe_future(h, klass));
  } catch (const std::runtime_error& e) {
    return map_runtime_error(e);
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API rmi_error rmi_client_instantiate(
    rmi_client_handle h,
    uint32_t klass,
    bison_handle params,
    rmi_proxy_handle* out_proxy) {
  if (!out_proxy)
    return RMI_ERR_NULL;
  if (!client_handle_is_valid(h))
    return RMI_ERR_NULL;
  try {
    proxy::dynamic proxy_obj = make_instantiate_future(h, klass, params).get();
    auto* pp =
        new proxy_ptr(std::make_unique<proxy::dynamic>(std::move(proxy_obj)));
    *out_proxy = as_proxy_handle(pp);
    return RMI_OK;
  } catch (const std::runtime_error&) {
    return RMI_ERR_INVALID_STATE;
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API rmi_error rmi_client_instantiate_async(
    rmi_client_handle h,
    uint32_t klass,
    bison_handle params,
    rmi_future_handle* out_future) {
  if (!client_handle_is_valid(h))
    return RMI_ERR_NULL;
  try {
    return store_future_handle<proxy_future_state>(
        out_future, make_instantiate_future(h, klass, params));
  } catch (const std::runtime_error& e) {
    return map_runtime_error(e);
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API rmi_error rmi_client_disconnect(rmi_client_handle h) {
  if (!client_handle_is_valid(h))
    return RMI_ERR_NULL;
  // Standalone sessions have no transport to disconnect.
  if (standalone_deref(h))
    return RMI_OK;
  client* c = client_deref(h);
  if (!c)
    return RMI_ERR_NULL;
  try {
    c->disconnect();
    return RMI_OK;
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API void rmi_client_release(rmi_client_handle h) {
  if (!h)
    return;
  client_state* state = as_client_state(h);
  if (!state || state->released)
    return;

  try {
    if (!state->is_standalone()) {
      client* c = client_deref(h);
      if (c)
        c->disconnect();
    }
    // Standalone session is cleaned up by its destructor.
  } catch (...) {
    // Suppress exceptions during cleanup
  }

  state->released = true;
  if (state->owns_resource || state->is_standalone())
    delete state;
}

// ─── Proxy ────────────────────────────────────────────────────────────────

RMI_API void rmi_proxy_release(rmi_proxy_handle proxy) {
  if (!proxy)
    return;
  delete as_proxy_ptr(proxy);
}

RMI_API rmi_error rmi_proxy_on_event(
    rmi_proxy_handle proxy,
    uint32_t event_name,
    rmi_proxy_event_fn handler,
    void* user) {
  proxy::dynamic* px = proxy_deref(proxy);
  if (!px || !handler)
    return RMI_ERR_NULL;

  try {
    px->onEvent(bdg::bison::key_t{event_name}, [handler, user](dynamic params) {
      auto sp = std::make_shared<dynamic>(std::move(params));
      bison_dynamic_ptr holder(sp);
      bison_handle temp_h = as_bison_handle(&holder);
      handler(temp_h, user);
    });
    return RMI_OK;
  } catch (const std::runtime_error& e) {
    return map_runtime_error(e);
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API rmi_error rmi_proxy_clear(
    rmi_client_handle h,
    rmi_proxy_handle proxy,
    int64_t timeout_ms) {
  if (!client_handle_is_valid(h))
    return RMI_ERR_NULL;
  proxy::dynamic* px = proxy_deref(proxy);
  if (!px)
    return RMI_ERR_NULL;

  try {
    std::future<bool> fut = make_proxy_clear_future(px);
    return wait_future_bool(fut, timeout_ms);
  } catch (const std::runtime_error& e) {
    return map_runtime_error(e);
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API rmi_error rmi_proxy_clear_async(
    rmi_client_handle h,
    rmi_proxy_handle proxy,
    rmi_future_handle* out_future) {
  if (!client_handle_is_valid(h))
    return RMI_ERR_NULL;
  proxy::dynamic* px = proxy_deref(proxy);
  if (!px)
    return RMI_ERR_NULL;

  try {
    return store_future_handle<bool_future_state>(
        out_future, make_proxy_clear_future(px));
  } catch (const std::runtime_error& e) {
    return map_runtime_error(e);
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API rmi_error rmi_proxy_set(
    rmi_client_handle h,
    rmi_proxy_handle proxy,
    bison_handle fields,
    int64_t timeout_ms) {
  if (!client_handle_is_valid(h))
    return RMI_ERR_NULL;
  proxy::dynamic* px = proxy_deref(proxy);
  if (!px)
    return RMI_ERR_NULL;

  try {
    std::future<bool> fut = make_proxy_set_future(px, fields);
    return wait_future_bool(fut, timeout_ms);
  } catch (const std::runtime_error& e) {
    return map_runtime_error(e);
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API rmi_error rmi_proxy_set_async(
    rmi_client_handle h,
    rmi_proxy_handle proxy,
    bison_handle fields,
    rmi_future_handle* out_future) {
  if (!client_handle_is_valid(h))
    return RMI_ERR_NULL;
  proxy::dynamic* px = proxy_deref(proxy);
  if (!px)
    return RMI_ERR_NULL;

  try {
    return store_future_handle<bool_future_state>(
        out_future, make_proxy_set_future(px, fields));
  } catch (const std::runtime_error& e) {
    return map_runtime_error(e);
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API rmi_error rmi_proxy_get(
    rmi_client_handle h,
    rmi_proxy_handle proxy,
    bison_handle projection,
    bison_handle* out_result,
    int64_t timeout_ms) {
  if (!out_result)
    return RMI_ERR_NULL;
  if (!client_handle_is_valid(h))
    return RMI_ERR_NULL;
  proxy::dynamic* px = proxy_deref(proxy);
  if (!px)
    return RMI_ERR_NULL;

  try {
    std::future<dynamic> fut = make_proxy_get_future(px, projection);

    dynamic result;
    rmi_error err = wait_future_result(fut, timeout_ms, &result);
    if (err != RMI_OK)
      return err;

    *out_result = dynamic_to_bison_handle(std::move(result));
    return RMI_OK;
  } catch (const std::runtime_error& e) {
    return map_runtime_error(e);
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API rmi_error rmi_proxy_get_async(
    rmi_client_handle h,
    rmi_proxy_handle proxy,
    bison_handle projection,
    rmi_future_handle* out_future) {
  if (!client_handle_is_valid(h))
    return RMI_ERR_NULL;
  proxy::dynamic* px = proxy_deref(proxy);
  if (!px)
    return RMI_ERR_NULL;

  try {
    return store_future_handle<dynamic_future_state>(
        out_future, make_proxy_get_future(px, projection));
  } catch (const std::runtime_error& e) {
    return map_runtime_error(e);
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API rmi_error rmi_proxy_call(
    rmi_client_handle h,
    rmi_proxy_handle proxy,
    uint32_t method,
    bison_handle params,
    bison_handle* out_result,
    int64_t timeout_ms) {
  if (!client_handle_is_valid(h))
    return RMI_ERR_NULL;
  proxy::dynamic* px = proxy_deref(proxy);
  if (!px)
    return RMI_ERR_NULL;

  try {
    std::future<dynamic> fut = make_proxy_call_future(px, method, params);

    dynamic result;
    rmi_error err = wait_future_result(fut, timeout_ms, &result);
    if (err != RMI_OK)
      return err;

    if (out_result) {
      *out_result = dynamic_to_bison_handle(std::move(result));
    }
    return RMI_OK;
  } catch (const std::runtime_error& e) {
    return map_runtime_error(e);
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API rmi_error rmi_proxy_call_async(
    rmi_client_handle h,
    rmi_proxy_handle proxy,
    uint32_t method,
    bison_handle params,
    rmi_future_handle* out_future) {
  if (!client_handle_is_valid(h))
    return RMI_ERR_NULL;
  proxy::dynamic* px = proxy_deref(proxy);
  if (!px)
    return RMI_ERR_NULL;

  try {
    return store_future_handle<dynamic_future_state>(
        out_future, make_proxy_call_future(px, method, params));
  } catch (const std::runtime_error& e) {
    return map_runtime_error(e);
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

// ─── Server ───────────────────────────────────────────────────────────────

RMI_API rmi_server_handle
rmi_server_tcp_create(const char* host, uint16_t port) {
  if (!host)
    return nullptr;
  try {
    auto* sp = new server_ptr(
        std::make_unique<server>(
            std::make_unique<socket_server_transport>(host, port)));
    return as_server_handle(sp);
  } catch (...) {
    return nullptr;
  }
}

RMI_API rmi_server_handle rmi_server_stdio_create(void) {
  try {
    auto* sp = new server_ptr(
        std::make_unique<server>(std::make_unique<stdio_server_transport>()));
    return as_server_handle(sp);
  } catch (...) {
    return nullptr;
  }
}

RMI_API rmi_error rmi_server_listen(rmi_server_handle h, bison_handle params) {
  server* s = server_deref(h);
  if (!s)
    return RMI_ERR_NULL;
  (void)params; // params not yet supported via C API
  try {
    s->listen(dynamic{});
    return RMI_OK;
  } catch (const std::runtime_error&) {
    return RMI_ERR_TRANSPORT;
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API void rmi_server_stop(rmi_server_handle h) {
  server* s = server_deref(h);
  if (!s)
    return;
  try {
    s->stop();
  } catch (...) {
    // Suppress exceptions during cleanup
  }
}

RMI_API void rmi_server_release(rmi_server_handle h) {
  if (!h)
    return;
  try {
    server* s = server_deref(h);
    if (s)
      s->stop();
  } catch (...) {
    // Suppress exceptions during cleanup
  }
  delete as_server_ptr(h);
}

RMI_API rmi_error rmi_pty_client_run(
    int argc,
    char** argv,
    const rmi_pty_client_callbacks* callbacks) {
  if (argc > 0 && !argv)
    return RMI_ERR_NULL;
  if (!callbacks || !callbacks->on_session)
    return RMI_ERR_NULL;

  try {
    c_pty_client_application_with_callbacks app(callbacks);
    return map_app_exit_code(app.run(argc, argv));
  } catch (const std::runtime_error& e) {
    return map_runtime_error(e);
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API rmi_error rmi_pty_server_run(
    int argc,
    char** argv,
    const rmi_pty_server_callbacks* callbacks) {
  if (argc > 0 && !argv)
    return RMI_ERR_NULL;
  if (!callbacks || !callbacks->register_classes)
    return RMI_ERR_NULL;

  try {
    c_pty_server_application_with_callbacks app(callbacks);
    return map_app_exit_code(app.run(argc, argv));
  } catch (const std::runtime_error& e) {
    return map_runtime_error(e);
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}
