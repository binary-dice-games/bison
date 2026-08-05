// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file rmi.hpp
 * @brief Header-only C++ wrapper around `rmi_c.h`, mirroring the internal
 *        `bdg::bison::rmi` client/server/proxy vocabulary
 *        (`src/rmi/client/client.hpp`, `src/rmi/client/proxy.hpp`,
 *        `src/rmi/server/server.hpp`) as closely as the C ABI allows.
 *
 * Requires `dynamic.hpp` (`bdg::bison::abi::dynamic`, `key_t`) and lives in
 * `bdg::bison::abi::rmi` for the same reason `dynamic.hpp` lives in
 * `bdg::bison::abi` rather than `bdg::bison` — see that header's top-level
 * doc comment for the (tested, reproduced) ELF symbol-interposition hazard
 * this avoids against `libbison_abi.so`'s own exported `bdg::bison::rmi::*`
 * symbols.
 *
 * ## How closely this mirrors the internal API
 *
 * `client`, `server`, and `proxy` keep their internal names and the same
 * `connect()` / `instantiate()` / `get()` / `set()` / `clear()` / `call()`
 * / `onEvent()` vocabulary. Differences, all forced by what `rmi_c.h`
 * exposes rather than by choice:
 * - **No transport abstraction.** The internal `client`/`server` take a
 *   `transport::*_transport_iface` so any transport (including custom ones)
 *   can be plugged in. The ABI only exposes the four transports it compiles
 *   in directly: `client::tcp()` / `pipe()` / `term()` / `standalone()`
 *   (and `server::tcp()` / `pipe()` / `term()`), matching the factory
 *   methods the Python and C# bindings expose.
 * - **No virtual extension hooks, with one exception.** The internal
 *   `server` has a long list of `on_*` overridable hooks (`on_check_class`,
 *   `on_create_object`, session context hooks, tracing hooks, ...) for
 *   embedding RMI into a larger application. None of that is reachable
 *   through `rmi_c.h` except connection authentication
 *   (`auth_module_iface`, `src/rmi/server/auth.hpp`), exposed here as
 *   `listen()`'s optional `auth` parameter. This `server` otherwise only
 *   wraps `rmi_server_{tcp,pipe,term}_create` / `listen` / `stop`.
 *   Class/method registration still goes through plain
 *   `dynamic::addClass()` / `dynamic::addMethod()`, exactly as in the
 *   `rmi_*_abi_*_example.cpp`
 *   examples.
 * - **Synchronous by default, not `std::future`-returning.** The internal
 *   `client`/`proxy` return `std::future<T>` from every operation. The ABI
 *   exposes both a blocking call (`rmi_proxy_call`, ...) and a separate
 *   `_async` variant that hands back an opaque `rmi_future_handle`
 *   (`rmi_c.h`) — there is no ABI path to a real `std::future`. This binding
 *   mirrors that split directly: `proxy::call()` / `client::instantiate()`
 *   etc. block and return their result by value (matching
 *   `proxy.call(...).get()` at the typical internal call site), and
 *   `_async()` counterparts return a `future` wrapping `rmi_future_handle`
 *   with `wait()` / `get_dynamic()` / `get_proxy()`.
 * - **`proxy`, not `proxy::dynamic`.** The internal proxy type is nested
 *   (`rmi::proxy::dynamic`) so it can share the `proxy_backend` abstraction
 *   between `client` and `standalone`. The ABI has no such split — one
 *   `rmi_proxy_handle` shape covers both — so this binding flattens it to
 *   `rmi::proxy`, matching the Python/C# `Proxy` naming.
 * - **The destructor destroys.** The internal `proxy::dynamic`'s destructor
 *   is deliberately a no-op — dropping a proxy without an explicit
 *   `destroy()` call intentionally leaks the remote object rather than
 *   risk an implicit network round-trip during unwind. `rmi_proxy_release()`
 *   (`rmi_c.h`) *always* sends the destroy request, with no lower-level
 *   "release without destroying" entry point to opt out of that, so this
 *   binding's `proxy` destructor calls it automatically — matching the
 *   Python (`Proxy.__del__`) and C# (`Proxy` finalizer) bindings' RAII
 *   behavior, which make the same call for the same reason.
 */

#pragma once

#include "rmi_c.h"

#include "dynamic.hpp"
#include "exception.hpp"
#include "key.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bdg::bison::abi::rmi {

class client;
class server;

// ═══════════════════════════════════════════════════════════════════════════
// future — thin wrapper around rmi_future_handle
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief RAII wrapper around `rmi_future_handle`, returned by every
 *        `*_async()` operation.
 *
 * Consume exactly once via `get_dynamic()` or `get_proxy()` (matching
 * whichever the originating call promises — see each method's doc comment),
 * or discard by letting it go out of scope / calling `release()`.
 */
class future {
 public:
  explicit future(rmi_future_handle h) : handle_(h) {}

  future(const future&) = delete;
  future& operator=(const future&) = delete;

  future(future&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  future& operator=(future&& other) noexcept {
    if (this != &other) {
      release();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  ~future() {
    release();
  }

  /** @brief Block until the operation completes, without consuming the
   *         future. @param timeout_ms  Milliseconds, or -1 for the default. */
  void wait(int64_t timeout_ms = -1) {
    abi::detail::check(rmi_future_wait(handle_, timeout_ms), "future.wait");
  }

  /** @brief Consume the future from a `describe_async()` /
   *         `set_async()`-style call and return its `dynamic` result. */
  dynamic get_dynamic() {
    bison_handle out = nullptr;
    abi::detail::check(rmi_future_get_dynamic(&handle_, &out), "future.get_dynamic");
    return dynamic::adopt(out);
  }

  /** @brief Consume the future from an `instantiate_async()` call and
   *         return its `proxy` result. Defined below `proxy`. */
  inline class proxy get_proxy();

  /** @brief Release without consuming the result. Idempotent. */
  void release() {
    if (handle_) {
      rmi_future_release(handle_);
      handle_ = nullptr;
    }
  }

 private:
  rmi_future_handle handle_;
};

// ═══════════════════════════════════════════════════════════════════════════
// proxy
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Move-only RAII handle to a remote (or in-process standalone)
 *        object, backed by `rmi_proxy_handle`.
 *
 * `dynamic::operator[]`-style field reads/writes are deliberately *not*
 * provided here: unlike a local `dynamic`, every remote field access is a
 * network round-trip, so this type keeps that cost visible at call sites
 * (`proxy.get()["field"_key]`, `proxy.set(fields)`) rather than hiding it
 * behind an operator.
 */
class proxy {
 public:
  explicit proxy(rmi_proxy_handle h) : handle_(h) {}

  proxy(const proxy&) = delete;
  proxy& operator=(const proxy&) = delete;

  proxy(proxy&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  proxy& operator=(proxy&& other) noexcept {
    if (this != &other) {
      release();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  /** @brief Sends the destroy request for the remote object — see this
   *         header's top-level doc comment for why, unlike the internal
   *         `proxy::dynamic`, this is unconditional. */
  ~proxy() {
    release();
  }

  /** @brief True unless this instance has been moved from or released. */
  bool valid() const {
    return handle_ != nullptr;
  }

  /** @brief The underlying `rmi_proxy_handle`, for direct `rmi_c.h` calls. */
  rmi_proxy_handle native_handle() const {
    return handle_;
  }

  // ── Remote operations ───────────────────────────────────────────────

  /** @brief Retrieve a full snapshot of all fields from the remote object. */
  dynamic get(int64_t timeout_ms = -1) {
    bison_handle out = nullptr;
    abi::detail::check(rmi_proxy_get(handle_, nullptr, &out, timeout_ms), "proxy.get");
    return dynamic::adopt(out);
  }

  /** @brief Retrieve a projected subset of fields (GraphQL-style: only the
   *         members present in @p projection are filled in). */
  dynamic get(const dynamic& projection, int64_t timeout_ms = -1) {
    bison_handle out = nullptr;
    abi::detail::check(rmi_proxy_get(handle_, projection.native_handle(), &out, timeout_ms), "proxy.get");
    return dynamic::adopt(out);
  }

  /** @brief `get()` submitted asynchronously; consume with `future::get_dynamic()`. */
  future get_async() {
    rmi_future_handle out = nullptr;
    abi::detail::check(rmi_proxy_get_async(handle_, nullptr, &out), "proxy.get_async");
    return future(out);
  }
  /** @copydoc get_async() */
  future get_async(const dynamic& projection) {
    rmi_future_handle out = nullptr;
    abi::detail::check(rmi_proxy_get_async(handle_, projection.native_handle(), &out), "proxy.get_async");
    return future(out);
  }

  /** @brief Apply a partial field update without resetting unspecified fields. */
  void set(const dynamic& fields, int64_t timeout_ms = -1) {
    abi::detail::check(rmi_proxy_set(handle_, fields.native_handle(), timeout_ms), "proxy.set");
  }

  /** @brief `set()` submitted asynchronously; consume with `future::wait()`. */
  future set_async(const dynamic& fields) {
    rmi_future_handle out = nullptr;
    abi::detail::check(rmi_proxy_set_async(handle_, fields.native_handle(), &out), "proxy.set_async");
    return future(out);
  }

  /** @brief Reset explicitly-set fields back to prototype/inherited defaults. */
  void clear(int64_t timeout_ms = -1) {
    abi::detail::check(rmi_proxy_clear(handle_, timeout_ms), "proxy.clear");
  }

  /** @brief `clear()` submitted asynchronously; consume with `future::wait()`. */
  future clear_async() {
    rmi_future_handle out = nullptr;
    abi::detail::check(rmi_proxy_clear_async(handle_, &out), "proxy.clear_async");
    return future(out);
  }

  /** @brief Invoke a named remote method with @p params (default: empty). */
  dynamic call(key_t name, const dynamic& params, int64_t timeout_ms = -1) {
    bison_handle out = nullptr;
    abi::detail::check(rmi_proxy_call(handle_, name, params.native_handle(), &out, timeout_ms), "proxy.call");
    return dynamic::adopt(out);
  }
  /** @copydoc call(key_t, const dynamic&, int64_t) */
  dynamic call(key_t name, int64_t timeout_ms = -1) {
    return call(name, dynamic(), timeout_ms);
  }

  /** @brief `call()` submitted asynchronously; consume with `future::get_dynamic()`. */
  future call_async(key_t name, const dynamic& params) {
    rmi_future_handle out = nullptr;
    abi::detail::check(rmi_proxy_call_async(handle_, name, params.native_handle(), &out), "proxy.call_async");
    return future(out);
  }

  /**
   * @brief Register a handler for a named server-initiated event.
   *
   * The handler closure is kept alive for the process's lifetime, same as
   * `dynamic::addMethod()` — see `dynamic.hpp`'s `detail::method_registry`
   * doc comment for why.
   */
  void onEvent(key_t name, std::function<void(const dynamic&)> handler);

  /** @brief Explicitly destroy the remote object now, instead of waiting
   *         for the destructor. Safe to call at most once; a no-op if
   *         already released. */
  void destroy() {
    release();
  }

 private:
  friend class future;
  friend class client;

  void release() {
    if (handle_) {
      rmi_proxy_release(handle_);
      handle_ = nullptr;
    }
  }

  rmi_proxy_handle handle_;
};

namespace detail {

using event_fn = std::function<void(const dynamic&)>;

inline std::mutex& event_registry_mutex() {
  static std::mutex m;
  return m;
}
inline std::vector<std::shared_ptr<event_fn>>& event_registry() {
  static std::vector<std::shared_ptr<event_fn>> registry;
  return registry;
}
inline event_fn* register_event_fn(event_fn fn) {
  auto stored = std::make_shared<event_fn>(std::move(fn));
  std::lock_guard<std::mutex> lock(event_registry_mutex());
  event_registry().push_back(stored);
  return stored.get();
}
inline void event_trampoline(bison_handle params, void* user) {
  auto* fn = static_cast<event_fn*>(user);
  dynamic params_view = dynamic::borrow(params);
  try {
    (*fn)(params_view);
  } catch (...) {
    // C ABI boundary: exceptions must not propagate back into bison_abi.
  }
}

} // namespace detail

inline void proxy::onEvent(key_t name, std::function<void(const dynamic&)> handler) {
  auto* stored = detail::register_event_fn(std::move(handler));
  abi::detail::check(rmi_proxy_on_event(handle_, name, &detail::event_trampoline, stored), "proxy.onEvent");
}

inline proxy future::get_proxy() {
  rmi_proxy_handle out = nullptr;
  abi::detail::check(rmi_future_get_proxy(&handle_, &out), "future.get_proxy");
  return proxy(out);
}

// ═══════════════════════════════════════════════════════════════════════════
// client
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief RAII wrapper around `rmi_client_handle`.
 *
 * Construct via `tcp()`, `pipe()`, `term()`, or `standalone()`; call
 * `connect()` before issuing requests (a no-op for `standalone()` clients,
 * matching `rmi_client_connect()`'s documented behavior).
 */
class client {
 public:
  client(const client&) = delete;
  client& operator=(const client&) = delete;
  client(client&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  client& operator=(client&& other) noexcept {
    if (this != &other) {
      release();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  ~client() {
    release();
  }

  /** @brief Create a TCP socket client (not yet connected). */
  static client tcp(const std::string& host, uint16_t port) {
    return client(checked(rmi_client_tcp_create(host.c_str(), port), "rmi_client_tcp_create"));
  }

  /** @brief Create a named-pipe / Unix-domain-socket client (not yet connected). */
  static client pipe(const std::string& path) {
    return client(checked(rmi_client_pipe_create(path.c_str()), "rmi_client_pipe_create"));
  }

  /** @brief Create a terminal (OSC-99 framed) client wrapping this
   *  process's inherited stdio. */
  static client term() {
    return client(checked(rmi_client_term_create(), "rmi_client_term_create"));
  }

  /** @brief Create an in-process client dispatching directly to the local
   *  class registry (`connect()`/`disconnect()` are no-ops). */
  static client standalone() {
    return client(checked(rmi_standalone_create(), "rmi_standalone_create"));
  }

  /** @brief Open the transport and start the client worker loop. */
  void connect(const dynamic& params = dynamic()) {
    abi::detail::check(rmi_client_connect(handle_, params.native_handle()), "connect");
  }

  /** @brief Gracefully disconnect and stop worker threads. */
  void disconnect() {
    abi::detail::check(rmi_client_disconnect(handle_), "disconnect");
  }

  /** @brief Request class metadata. @p klass = `key_t{0U}` requests full metadata. */
  dynamic describe(key_t ns = key_t{0U}, key_t klass = key_t{0U}) {
    bison_handle out = nullptr;
    abi::detail::check(rmi_client_describe(handle_, ns, klass, &out), "describe");
    return dynamic::adopt(out);
  }

  /** @brief `describe()` submitted asynchronously; consume with `future::get_dynamic()`. */
  future describe_async(key_t ns = key_t{0U}, key_t klass = key_t{0U}) {
    rmi_future_handle out = nullptr;
    abi::detail::check(rmi_client_describe_async(handle_, ns, klass, &out), "describe_async");
    return future(out);
  }

  /** @brief Instantiate a remote object and return a `proxy` to it. */
  proxy instantiate(key_t ns, key_t klass, const dynamic& params = dynamic()) {
    rmi_proxy_handle out = nullptr;
    abi::detail::check(rmi_client_instantiate(handle_, ns, klass, params.native_handle(), &out), "instantiate");
    return proxy(out);
  }
  /** @brief `instantiate()` in the global namespace. */
  proxy instantiate(key_t klass, const dynamic& params = dynamic()) {
    return instantiate(key_t{0U}, klass, params);
  }

  /** @brief `instantiate()` submitted asynchronously; consume with `future::get_proxy()`. */
  future instantiate_async(key_t ns, key_t klass, const dynamic& params = dynamic()) {
    rmi_future_handle out = nullptr;
    abi::detail::check(
        rmi_client_instantiate_async(handle_, ns, klass, params.native_handle(), &out), "instantiate_async");
    return future(out);
  }
  /** @copydoc instantiate_async(key_t, key_t, const dynamic&) */
  future instantiate_async(key_t klass, const dynamic& params = dynamic()) {
    return instantiate_async(key_t{0U}, klass, params);
  }

  /** @brief Destroy a remote object represented by @p p, matching the
   *         internal `client::destroy(proxy::dynamic&&)`. Equivalent to
   *         letting @p p go out of scope, spelled out for call sites that
   *         want to be explicit about when the destroy happens. */
  void destroy(proxy&& p) {
    p.destroy();
  }

  /** @brief The underlying `rmi_client_handle`, for direct `rmi_c.h` calls. */
  rmi_client_handle native_handle() const {
    return handle_;
  }

 private:
  explicit client(rmi_client_handle h) : handle_(h) {}

  static rmi_client_handle checked(rmi_client_handle h, const char* context) {
    if (!h)
      throw std::runtime_error(std::string(context) + " failed");
    return h;
  }

  void release() {
    if (handle_) {
      rmi_client_release(handle_);
      handle_ = nullptr;
    }
  }

  rmi_client_handle handle_;
};

// ═══════════════════════════════════════════════════════════════════════════
// server
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief RAII wrapper around `rmi_server_handle`.
 *
 * Construct via `tcp()`, `pipe()`, or `term()`, register classes/methods
 * with plain `dynamic::addClass()` / `dynamic::addMethod()` beforehand
 * (see this header's top-level doc comment for why there is no separate
 * RMI-specific registration API), then `listen()`.
 */
class server {
 public:
  server(const server&) = delete;
  server& operator=(const server&) = delete;
  server(server&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  server& operator=(server&& other) noexcept {
    if (this != &other) {
      release();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  ~server() {
    release();
  }

  /** @brief Create a TCP socket server listener (not yet listening). */
  static server tcp(const std::string& host, uint16_t port) {
    return server(checked(rmi_server_tcp_create(host.c_str(), port), "rmi_server_tcp_create"));
  }

  /** @brief Create a named-pipe / Unix-domain-socket server listener (not
   *  yet listening). */
  static server pipe(const std::string& path) {
    return server(checked(rmi_server_pipe_create(path.c_str()), "rmi_server_pipe_create"));
  }

  /** @brief Create a terminal (OSC-99 framed) server listener, spawning
   *  @p cmd (or the operator's default shell if empty). */
  static server term(const std::string& cmd = {}) {
    return server(checked(rmi_server_term_create(cmd.empty() ? nullptr : cmd.c_str()), "rmi_server_term_create"));
  }

  /**
   * @brief Start accepting client connections and spawn worker threads.
   *
   * @p auth, if given, is evaluated once per incoming connection for as
   * long as the server keeps listening -- matching the internal
   * `server::listen()`'s `auth_module` parameter, it can only be set here,
   * not changed afterward. It receives the client's `OP_CONNECT` payload
   * and returns `true` to accept the connection (optionally writing an
   * identity string into the `std::string&` out-parameter) or `false` to
   * reject it. The handler closure is kept alive for the process's
   * lifetime, same as `proxy::onEvent()` — see `detail::event_registry`'s
   * doc comment.
   */
  void listen(
      const dynamic& params = dynamic(),
      std::function<bool(const dynamic& payload, std::string& out_identity)> auth = nullptr);

  /** @brief Stop the listener and let active workers finish. */
  void stop() {
    rmi_server_stop(handle_);
  }

  /** @brief The underlying `rmi_server_handle`, for direct `rmi_c.h` calls. */
  rmi_server_handle native_handle() const {
    return handle_;
  }

 private:
  explicit server(rmi_server_handle h) : handle_(h) {}

  static rmi_server_handle checked(rmi_server_handle h, const char* context) {
    if (!h)
      throw std::runtime_error(std::string(context) + " failed");
    return h;
  }

  void release() {
    if (handle_) {
      rmi_server_release(handle_);
      handle_ = nullptr;
    }
  }

  rmi_server_handle handle_;
};

namespace detail {

using auth_fn = std::function<bool(const dynamic&, std::string&)>;

inline std::mutex& auth_registry_mutex() {
  static std::mutex m;
  return m;
}
inline std::vector<std::shared_ptr<auth_fn>>& auth_registry() {
  static std::vector<std::shared_ptr<auth_fn>> registry;
  return registry;
}
inline auth_fn* register_auth_fn(auth_fn fn) {
  auto stored = std::make_shared<auth_fn>(std::move(fn));
  std::lock_guard<std::mutex> lock(auth_registry_mutex());
  auth_registry().push_back(stored);
  return stored.get();
}
inline bool auth_trampoline(bison_handle payload, char* identity_buf, size_t identity_buf_len, void* user) {
  auto* fn = static_cast<auth_fn*>(user);
  dynamic payload_view = dynamic::borrow(payload);
  std::string identity;
  bool accepted = false;
  try {
    accepted = (*fn)(payload_view, identity);
  } catch (...) {
    // C ABI boundary: exceptions must not propagate back into bison_abi.
    return false;
  }
  if (accepted && identity_buf_len > 0) {
    size_t n = std::min(identity.size(), identity_buf_len - 1);
    std::memcpy(identity_buf, identity.data(), n);
    identity_buf[n] = '\0';
  }
  return accepted;
}

} // namespace detail

inline void
server::listen(const dynamic& params, std::function<bool(const dynamic&, std::string&)> auth) {
  if (auth) {
    auto* stored = detail::register_auth_fn(std::move(auth));
    abi::detail::check(
        rmi_server_listen(handle_, params.native_handle(), &detail::auth_trampoline, stored), "listen");
  } else {
    abi::detail::check(rmi_server_listen(handle_, params.native_handle(), nullptr, nullptr), "listen");
  }
}

} // namespace bdg::bison::abi::rmi
