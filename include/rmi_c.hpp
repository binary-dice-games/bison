// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file rmi_c.hpp
 * @brief C++ RAII wrappers for the Bison RMI C ABI (`rmi_c.h`).
 *
 * Provides `bdg::rmi_c::future`, `bdg::rmi_c::client`,
 * `bdg::rmi_c::server`, and `bdg::rmi_c::proxy` — thin, header-only RAII
 * classes that sit on top of the stable C ABI and offer a more idiomatic
 * C++ interface:
 *
 * - Handles are released automatically in destructors.
 * - Async futures are move-only and discarded via `rmi_future_release` if
 *   not consumed.
 * - `bison_handle` in/out parameters are replaced with `bison_c::object`.
 * - All errors surface as `std::runtime_error` exceptions.
 *
 * Because every call goes through the C ABI, these wrappers are safe to use
 * from a different DLL — no C++ vtables, `std::string` instances, or
 * allocator state are shared with the RMI shared library.
 *
 * ### Example
 * @code{.cpp}
 * #include "rmi_c.hpp"
 * using namespace bdg::rmi_c;
 *
 * auto c = client::tcp("127.0.0.1", 8080);
 * c.connect();
 * proxy p = c.instantiate(bison_c::object::key("MyClass"));
 * bison_c::object result =
 *     p.call(c, bison_c::object::key("greet"), bison_c::object::create());
 * @endcode
 */

#pragma once

#include "bison_c.hpp"
#include "rmi_c.h"

#include <stdexcept>
#include <string>

namespace bdg::rmi_c {

namespace detail {

inline void check(rmi_error err, const char* msg) {
  if (err != RMI_OK) {
    throw std::runtime_error(
        std::string(msg) + " (code " + std::to_string(static_cast<int>(err)) +
        ")");
  }
}

} // namespace detail

/** @brief Convenience alias for the bison object wrapper. */
using object = bdg::bison_c::object;

// ────────────────────────────────────────────────────────────────────────────
// future — RAII wrapper for rmi_future_handle
// ────────────────────────────────────────────────────────────────────────────

/**
 * @brief RAII owner of an `rmi_future_handle`.
 *
 * Move-only.  If not consumed before destruction the handle is discarded
 * via `rmi_future_release`.
 *
 * Consume a future by calling `get_dynamic()` (for operations that return a
 * `bison_handle` result) or `get_proxy()` (for operations that return a
 * remote object proxy).  Both consuming calls set the internal handle to
 * null so the future cannot be consumed twice.
 */
class future {
 public:
  /** @brief Construct a null (invalid) future. */
  future() noexcept = default;

  /** @brief Take ownership of a raw future handle. */
  static future own(rmi_future_handle h) noexcept {
    return future(h);
  }

  ~future() {
    rmi_future_release(f_);
  }

  future(const future&) = delete;
  future& operator=(const future&) = delete;

  future(future&& o) noexcept : f_(o.f_) {
    o.f_ = nullptr;
  }

  future& operator=(future&& o) noexcept {
    if (this != &o) {
      rmi_future_release(f_);
      f_ = o.f_;
      o.f_ = nullptr;
    }
    return *this;
  }

  /** @brief `true` if the future handle is non-null. */
  explicit operator bool() const noexcept {
    return f_ != nullptr;
  }

  /**
   * @brief Wait for the future to become ready without consuming it.
   *
   * @param timeout_ms  Timeout in milliseconds; `-1` for the default.
   * @throws std::runtime_error on timeout or error.
   */
  void wait(int64_t timeout_ms = -1) const {
    detail::check(rmi_future_wait(f_, timeout_ms), "rmi_future_wait");
  }

  /**
   * @brief Consume the future and return its `dynamic` result.
   *
   * After this call the internal handle is null.
   *
   * @return Owning `object` wrapping the result value.
   * @throws std::runtime_error on error.
   */
  object get_dynamic() {
    bison_handle out = nullptr;
    detail::check(rmi_future_get_dynamic(&f_, &out), "rmi_future_get_dynamic");
    return object::own(out);
  }

  /** @brief Return the raw handle without transferring ownership. */
  rmi_future_handle get() const noexcept {
    return f_;
  }

 private:
  rmi_future_handle f_ = nullptr;
  explicit future(rmi_future_handle f) noexcept : f_(f) {}
};

// Forward declaration needed by proxy methods.
class client;

// ────────────────────────────────────────────────────────────────────────────
// proxy — RAII wrapper for rmi_proxy_handle
// ────────────────────────────────────────────────────────────────────────────

/**
 * @brief RAII owner of an `rmi_proxy_handle`.
 *
 * Move-only.  Sends a destroy request to the server via `rmi_proxy_release`
 * on destruction.
 *
 * Proxy operations that require a live client connection (`clear`, `set`,
 * `get`, `call`, and their async variants) accept a `client&` parameter so
 * the dependency is explicit and the client lifetime is caller-managed.
 */
class proxy {
 public:
  /** @brief Construct a null (invalid) proxy. */
  proxy() noexcept = default;

  /** @brief Take ownership of a raw proxy handle. */
  static proxy own(rmi_proxy_handle h) noexcept {
    return proxy(h);
  }

  ~proxy() {
    rmi_proxy_release(p_);
  }

  proxy(const proxy&) = delete;
  proxy& operator=(const proxy&) = delete;

  proxy(proxy&& o) noexcept : p_(o.p_) {
    o.p_ = nullptr;
  }

  proxy& operator=(proxy&& o) noexcept {
    if (this != &o) {
      rmi_proxy_release(p_);
      p_ = o.p_;
      o.p_ = nullptr;
    }
    return *this;
  }

  /** @brief `true` if the proxy handle is non-null. */
  explicit operator bool() const noexcept {
    return p_ != nullptr;
  }

  /** @brief Return the raw handle without transferring ownership. */
  rmi_proxy_handle get() const noexcept {
    return p_;
  }

  // ── Event subscription ────────────────────────────────────────────────────

  /**
   * @brief Subscribe to a server-initiated event on this proxy.
   *
   * @param event_name  Hashed event name (use `object::key()`).
   * @param handler     Callback invoked on the client's worker thread.
   * @param user        Arbitrary context pointer passed to @p handler.
   * @throws std::runtime_error on error.
   */
  void on_event(
      uint32_t event_name,
      rmi_proxy_event_fn handler,
      void* user = nullptr) {
    detail::check(
        rmi_proxy_on_event(p_, event_name, handler, user),
        "rmi_proxy_on_event");
  }

  // ── Client-bound operations (defined after `client`) ─────────────────────

  /**
   * @brief Clear explicitly set fields on the remote object (synchronous).
   *
   * @param c           Connected client that owns this proxy's session.
   * @param timeout_ms  Timeout in milliseconds; `-1` for the default.
   * @throws std::runtime_error on error.
   */
  void clear(client& c, int64_t timeout_ms = -1);

  /**
   * @brief Clear explicitly set fields on the remote object (asynchronous).
   *
   * @param c           Connected client that owns this proxy's session.
   * @return Future waited on with `future::wait()` and released on
   *         destruction.
   * @throws std::runtime_error on submission failure.
   */
  future clear_async(client& c);

  /**
   * @brief Apply a partial field update to the remote object (synchronous).
   *
   * @param c           Connected client that owns this proxy's session.
   * @param fields      Object containing the fields to apply.
   * @param timeout_ms  Timeout in milliseconds; `-1` for the default.
   * @throws std::runtime_error on error.
   */
  void set(client& c, const object& fields, int64_t timeout_ms = -1);

  /**
   * @brief Apply a partial field update asynchronously.
   *
   * @param c       Connected client that owns this proxy's session.
   * @param fields  Object containing the fields to apply.
   * @return Future waited on with `future::wait()`.
   * @throws std::runtime_error on submission failure.
   */
  future set_async(client& c, const object& fields);

  /**
   * @brief Retrieve fields from the remote object (synchronous).
   *
   * @param c           Connected client that owns this proxy's session.
   * @param projection  Optional projection template; pass a
   *                    default-constructed `object` for a full snapshot.
   * @param timeout_ms  Timeout in milliseconds; `-1` for the default.
   * @return Owning result object.
   * @throws std::runtime_error on error.
   */
  object
  get(client& c, const object& projection = object{}, int64_t timeout_ms = -1);

  /**
   * @brief Retrieve fields from the remote object asynchronously.
   *
   * @param c           Connected client that owns this proxy's session.
   * @param projection  Optional projection template.
   * @return Future consumed with `future::get_dynamic()`.
   * @throws std::runtime_error on submission failure.
   */
  future get_async(client& c, const object& projection = object{});

  /**
   * @brief Call a method on the remote object (synchronous).
   *
   * @param c           Connected client that owns this proxy's session.
   * @param method      Method name hash (use `object::key()`).
   * @param params      Method arguments; pass a default-constructed `object`
   *                    for none.
   * @param timeout_ms  Timeout in milliseconds; `-1` for no timeout.
   * @return Result object (caller owns).
   * @throws std::runtime_error on error.
   */
  object call(
      client& c,
      uint32_t method,
      const object& params = object{},
      int64_t timeout_ms = -1);

  /**
   * @brief Call a method on the remote object asynchronously.
   *
   * @param c       Connected client that owns this proxy's session.
   * @param method  Method name hash (use `object::key()`).
   * @param params  Method arguments; pass a default-constructed `object`
   *                for none.
   * @return Future consumed with `future::get_dynamic()`.
   * @throws std::runtime_error on submission failure.
   */
  future
  call_async(client& c, uint32_t method, const object& params = object{});

 private:
  rmi_proxy_handle p_ = nullptr;
  explicit proxy(rmi_proxy_handle p) noexcept : p_(p) {}
};

// ────────────────────────────────────────────────────────────────────────────
// client — RAII wrapper for rmi_client_handle
// ────────────────────────────────────────────────────────────────────────────

/**
 * @brief RAII owner of an `rmi_client_handle`.
 *
 * Move-only.  Disconnects and releases the handle on destruction.
 *
 * Use the static factory `tcp()` to create a TCP/socket client.
 * Call `connect()` before issuing any requests.
 */
class client {
 public:
  /**
   * @brief Create a TCP socket client (not yet connected).
   *
   * @param host  Server hostname or IP address.
   * @param port  Server port number.
   * @throws std::runtime_error on allocation failure.
   */
  static client tcp(const char* host, uint16_t port) {
    rmi_client_handle h = rmi_client_tcp_create(host, port);
    if (!h)
      throw std::runtime_error("rmi_client_tcp_create failed");
    return client(h);
  }

  /** @brief Construct a null (invalid) client. */
  client() noexcept = default;

  ~client() {
    rmi_client_release(h_);
  }

  client(const client&) = delete;
  client& operator=(const client&) = delete;

  client(client&& o) noexcept : h_(o.h_) {
    o.h_ = nullptr;
  }

  client& operator=(client&& o) noexcept {
    if (this != &o) {
      rmi_client_release(h_);
      h_ = o.h_;
      o.h_ = nullptr;
    }
    return *this;
  }

  /** @brief `true` if the handle is non-null. */
  explicit operator bool() const noexcept {
    return h_ != nullptr;
  }

  /**
   * @brief Connect to the server and start the background worker thread.
   *
   * @param params  Optional connection parameters; pass a
   *                default-constructed `object` for none.
   * @throws std::runtime_error on failure.
   */
  void connect(const object& params = object{}) {
    detail::check(rmi_client_connect(h_, params.get()), "rmi_client_connect");
  }

  /**
   * @brief Disconnect from the server and stop the worker thread.
   * @throws std::runtime_error on failure.
   */
  void disconnect() {
    detail::check(rmi_client_disconnect(h_), "rmi_client_disconnect");
  }

  /**
   * @brief Request class metadata from the server (synchronous).
   *
   * @param klass  Class key (use `object::key()`); `0` for all metadata.
   * @return Owning descriptor object.
   * @throws std::runtime_error on failure.
   */
  object describe(uint32_t klass = 0) const {
    bison_handle out = nullptr;
    detail::check(rmi_client_describe(h_, klass, &out), "rmi_client_describe");
    return object::own(out);
  }

  /**
   * @brief Request class metadata asynchronously.
   *
   * @param klass  Class key; `0` for all metadata.
   * @return Future consumed with `future::get_dynamic()`.
   * @throws std::runtime_error on submission failure.
   */
  future describe_async(uint32_t klass = 0) const {
    rmi_future_handle f = nullptr;
    detail::check(
        rmi_client_describe_async(h_, klass, &f), "rmi_client_describe_async");
    return future::own(f);
  }

  /**
   * @brief Instantiate a remote object (synchronous).
   *
   * @param klass   Class key (use `object::key()`).
   * @param params  Constructor arguments; pass a default-constructed `object`
   *                for none.
   * @return Owning proxy for the remote object.
   * @throws std::runtime_error on failure.
   */
  proxy instantiate(uint32_t klass, const object& params = object{}) {
    rmi_proxy_handle p = nullptr;
    detail::check(
        rmi_client_instantiate(h_, klass, params.get(), &p),
        "rmi_client_instantiate");
    return proxy::own(p);
  }

  /**
   * @brief Instantiate a remote object asynchronously.
   *
   * Consume the returned future with `future::get_proxy()` to obtain the
   * owning `proxy`.
   *
   * @param klass   Class key.
   * @param params  Constructor arguments.
   * @return Future consumed with `future::get_proxy()`.
   * @throws std::runtime_error on submission failure.
   */
  future instantiate_async(uint32_t klass, const object& params = object{}) {
    rmi_future_handle f = nullptr;
    detail::check(
        rmi_client_instantiate_async(h_, klass, params.get(), &f),
        "rmi_client_instantiate_async");
    return future::own(f);
  }

  /** @brief Return the raw handle without transferring ownership. */
  rmi_client_handle get() const noexcept {
    return h_;
  }

 private:
  rmi_client_handle h_ = nullptr;
  explicit client(rmi_client_handle h) noexcept : h_(h) {}
};

// ── future::get_proxy — defined after proxy and client ───────────────────

/**
 * @brief Consume the future and return its proxy result.
 *
 * After this call the internal handle is null.
 *
 * @return Owning `proxy` wrapping the remote object.
 * @throws std::runtime_error on error.
 */
inline proxy future_get_proxy(future& f) {
  rmi_proxy_handle out = nullptr;
  rmi_future_handle raw = f.get();
  rmi_error err = rmi_future_get_proxy(&raw, &out);
  // Sync the consumed handle back into the future wrapper.
  // The future's internal handle has been set to null by rmi_future_get_proxy.
  f = future{}; // discard; the C function already nulled the raw handle
  detail::check(err, "rmi_future_get_proxy");
  return proxy::own(out);
}

// ── proxy out-of-line definitions ─────────────────────────────────────────

inline void proxy::clear(client& c, int64_t timeout_ms) {
  detail::check(rmi_proxy_clear(c.get(), p_, timeout_ms), "rmi_proxy_clear");
}

inline future proxy::clear_async(client& c) {
  rmi_future_handle f = nullptr;
  detail::check(
      rmi_proxy_clear_async(c.get(), p_, &f), "rmi_proxy_clear_async");
  return future::own(f);
}

inline void proxy::set(client& c, const object& fields, int64_t timeout_ms) {
  detail::check(
      rmi_proxy_set(c.get(), p_, fields.get(), timeout_ms), "rmi_proxy_set");
}

inline future proxy::set_async(client& c, const object& fields) {
  rmi_future_handle f = nullptr;
  detail::check(
      rmi_proxy_set_async(c.get(), p_, fields.get(), &f),
      "rmi_proxy_set_async");
  return future::own(f);
}

inline object
proxy::get(client& c, const object& projection, int64_t timeout_ms) {
  bison_handle out = nullptr;
  detail::check(
      rmi_proxy_get(c.get(), p_, projection.get(), &out, timeout_ms),
      "rmi_proxy_get");
  return object::own(out);
}

inline future proxy::get_async(client& c, const object& projection) {
  rmi_future_handle f = nullptr;
  detail::check(
      rmi_proxy_get_async(c.get(), p_, projection.get(), &f),
      "rmi_proxy_get_async");
  return future::own(f);
}

inline object proxy::call(
    client& c,
    uint32_t method,
    const object& params,
    int64_t timeout_ms) {
  bison_handle out = nullptr;
  detail::check(
      rmi_proxy_call(c.get(), p_, method, params.get(), &out, timeout_ms),
      "rmi_proxy_call");
  return object::own(out);
}

inline future
proxy::call_async(client& c, uint32_t method, const object& params) {
  rmi_future_handle f = nullptr;
  detail::check(
      rmi_proxy_call_async(c.get(), p_, method, params.get(), &f),
      "rmi_proxy_call_async");
  return future::own(f);
}

// ────────────────────────────────────────────────────────────────────────────
// server — RAII wrapper for rmi_server_handle
// ────────────────────────────────────────────────────────────────────────────

/**
 * @brief RAII owner of an `rmi_server_handle`.
 *
 * Move-only.  Stops (if listening) and releases the handle on destruction.
 *
 * Use the static factory `tcp()` to create a TCP/socket server.
 * Call `listen()` to begin accepting connections.
 */
class server {
 public:
  /**
   * @brief Create a TCP socket server (not yet listening).
   *
   * @param host  Bind address (e.g. `"0.0.0.0"` for all interfaces).
   * @param port  Bind port number.
   * @throws std::runtime_error on allocation failure.
   */
  static server tcp(const char* host, uint16_t port) {
    rmi_server_handle h = rmi_server_tcp_create(host, port);
    if (!h)
      throw std::runtime_error("rmi_server_tcp_create failed");
    return server(h);
  }

  /** @brief Construct a null (invalid) server. */
  server() noexcept = default;

  ~server() {
    rmi_server_release(h_);
  }

  server(const server&) = delete;
  server& operator=(const server&) = delete;

  server(server&& o) noexcept : h_(o.h_) {
    o.h_ = nullptr;
  }

  server& operator=(server&& o) noexcept {
    if (this != &o) {
      rmi_server_release(h_);
      h_ = o.h_;
      o.h_ = nullptr;
    }
    return *this;
  }

  /** @brief `true` if the handle is non-null. */
  explicit operator bool() const noexcept {
    return h_ != nullptr;
  }

  /**
   * @brief Start listening for incoming client connections.
   *
   * @param params  Optional listen parameters; pass a default-constructed
   *                `object` for none.
   * @throws std::runtime_error on failure.
   */
  void listen(const object& params = object{}) {
    detail::check(rmi_server_listen(h_, params.get()), "rmi_server_listen");
  }

  /**
   * @brief Stop accepting connections and close active sessions.
   *
   * Idempotent — safe to call multiple times or before destruction.
   */
  void stop() noexcept {
    if (h_)
      rmi_server_stop(h_);
  }

  /** @brief Return the raw handle without transferring ownership. */
  rmi_server_handle get() const noexcept {
    return h_;
  }

 private:
  rmi_server_handle h_ = nullptr;
  explicit server(rmi_server_handle h) noexcept : h_(h) {}
};

} // namespace bdg::rmi_c
