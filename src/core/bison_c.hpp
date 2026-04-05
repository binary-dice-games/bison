// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file bison_c.hpp
 * @brief C++ RAII wrappers for the Bison C ABI (`bison_c.h`).
 *
 * Provides `bdg::bison_c::object`, `bdg::bison_c::rmi_transport`,
 * `bdg::bison_c::rmi_server`, `bdg::bison_c::rmi_client`, and
 * `bdg::bison_c::rmi_proxy` — thin, header-only RAII classes that sit on
 * top of the stable C ABI and offer a more idiomatic C++ interface:
 *
 * - Handles are released automatically in destructors.
 * - Copying an `object` calls `bison_add_ref()` so reference counting is
 *   transparent to the caller.
 * - Scalar getters return values directly and throw `std::runtime_error` on
 *   failure instead of using out-parameters and error codes.
 * - String getters return `std::string`.
 * - Boolean fields use `bool` rather than `int`.
 *
 * Because every call goes through the C ABI, these wrappers are safe to use
 * from a different DLL or shared library — no C++ vtables, `std::string`
 * instances, or allocator state are shared with the bison shared library.
 *
 * ### Example
 * @code{.cpp}
 * #include "src/core/bison_c.hpp"
 * using namespace bdg::bison_c;
 *
 * auto score_obj = object::create(object::key("Player"));
 * score_obj.set_int(object::key("score"), 42);
 * int32_t score = score_obj.get_int(object::key("score"));  // 42
 * @endcode
 */

#pragma once

#include "src/core/bison_c.h"

#include <stdexcept>
#include <string>

namespace bdg::bison_c {

namespace detail {

inline void check(bison_error err, const char* msg) {
  if (err != BISON_OK) {
    throw std::runtime_error(
        std::string(msg) + " (code " + std::to_string(static_cast<int>(err)) +
        ")");
  }
}

} // namespace detail

// ────────────────────────────────────────────────────────────────────────────
// object — RAII wrapper for bison_handle
// ────────────────────────────────────────────────────────────────────────────

/**
 * @brief RAII owner of a `bison_handle`.
 *
 * Automatically releases the underlying handle in the destructor.  Copying
 * calls `bison_add_ref()` so both the original and the copy are independent
 * owners.  Moving leaves the source in the null (invalid) state.
 *
 * The default constructor produces a null object; check validity with
 * `operator bool()` before calling any getter or setter.
 */
class object {
 public:
  // ── Factories ─────────────────────────────────────────────────────────────

  /**
   * @brief Take ownership of an already-created raw handle.
   *
   * The caller must **not** call `bison_release()` on @p h after this.
   */
  static object own(bison_handle h) noexcept {
    return object(h);
  }

  /**
   * @brief Create an owning copy of a non-owning handle via `bison_add_ref`.
   *
   * Useful for handles returned by functions that do not transfer ownership,
   * such as `bison_find_class`.
   */
  static object borrow(bison_handle h) noexcept {
    return object(bison_add_ref(h));
  }

  /**
   * @brief Create a new, empty dynamic object.
   *
   * @param klass_name  Hashed class name (use `key()`); pass `0` for an
   *                    anonymous object.
   * @throws std::runtime_error on allocation failure.
   */
  static object create(bison_hash klass_name = 0) {
    bison_handle h = bison_create(klass_name);
    if (!h)
      throw std::runtime_error("bison_create failed");
    return object(h);
  }

  /**
   * @brief Instantiate a class-typed object in the global namespace.
   *
   * @param klass_name  Hashed class name (use `key()`).
   * @throws std::runtime_error on failure.
   */
  static object instantiate(bison_hash klass_name) {
    bison_handle h = bison_instantiate(klass_name);
    if (!h)
      throw std::runtime_error("bison_instantiate failed");
    return object(h);
  }

  /**
   * @brief Instantiate a class-typed object in a named namespace.
   *
   * @param ns_name     Hashed namespace name (use `key()`); `0` for global.
   * @param klass_name  Hashed class name (use `key()`).
   * @throws std::runtime_error on failure.
   */
  static object instantiate_ns(bison_hash ns_name, bison_hash klass_name) {
    bison_handle h = bison_instantiate_ns(ns_name, klass_name);
    if (!h)
      throw std::runtime_error("bison_instantiate_ns failed");
    return object(h);
  }

  /**
   * @brief Parse a JSON string into an object.
   *
   * @param json  Null-terminated UTF-8 JSON string.
   * @throws std::runtime_error on parse failure.
   */
  static object from_json(const char* json) {
    bison_handle h = bison_from_json(json);
    if (!h)
      throw std::runtime_error("bison_from_json: parse error");
    return object(h);
  }

  /**
   * @brief Parse a YAML string into an object.
   *
   * @param yaml  Null-terminated UTF-8 YAML string.
   * @throws std::runtime_error on parse failure.
   */
  static object from_yaml(const char* yaml) {
    bison_handle h = bison_from_yaml(yaml);
    if (!h)
      throw std::runtime_error("bison_from_yaml: parse error");
    return object(h);
  }

  // ── Lifecycle ─────────────────────────────────────────────────────────────

  /** @brief Construct a null (invalid) object. */
  object() noexcept = default;

  ~object() {
    reset();
  }

  /** @brief Copy: increments the reference count via `bison_add_ref`. */
  object(const object& o) noexcept : h_(bison_add_ref(o.h_)) {}

  /** @brief Move: takes the handle; source becomes null. */
  object(object&& o) noexcept : h_(o.h_) {
    o.h_ = nullptr;
  }

  object& operator=(const object& o) noexcept {
    if (this != &o) {
      reset();
      h_ = bison_add_ref(o.h_);
    }
    return *this;
  }

  object& operator=(object&& o) noexcept {
    if (this != &o) {
      reset();
      h_ = o.h_;
      o.h_ = nullptr;
    }
    return *this;
  }

  /** @brief Release the handle and set to null. */
  void reset() noexcept {
    bison_release(h_);
    h_ = nullptr;
  }

  /** @brief Release the current handle and take ownership of @p h. */
  void reset(bison_handle h) noexcept {
    bison_release(h_);
    h_ = h;
  }

  /** @brief Return the raw handle without transferring ownership. */
  bison_handle get() const noexcept {
    return h_;
  }

  /** @brief `true` if the handle is non-null. */
  explicit operator bool() const noexcept {
    return h_ != nullptr;
  }

  // ── Registry (static) ─────────────────────────────────────────────────────

  /** @brief Compute the FNV-1a hash of a null-terminated string. */
  static bison_hash key(const char* name) {
    return bison_key(name);
  }

  /**
   * @brief Register @p klass as a prototype in the global namespace.
   *
   * @param parent_name  Hashed parent class name (use `key()`); `0` for a
   *                     root class.
   * @param klass        Object whose `__class` field has already been set.
   * @throws std::runtime_error on null handle or duplicate class name.
   */
  static void add_class(bison_hash parent_name, const object& klass) {
    detail::check(bison_add_class(parent_name, klass.h_), "bison_add_class");
  }

  /**
   * @brief Register @p klass as a prototype in a named namespace.
   *
   * @param ns_name      Hashed namespace name (use `key()`); `0` for global.
   * @param klass        Object whose `__class` field has already been set.
   * @param parent_name  Hashed parent class name; `0` for a root class.
   * @throws std::runtime_error on null handle or duplicate class name.
   */
  static void add_class_ns(
      bison_hash ns_name,
      const object& klass,
      bison_hash parent_name) {
    detail::check(
        bison_add_class_ns(ns_name, klass.h_, parent_name),
        "bison_add_class_ns");
  }

  /**
   * @brief Walk the class hierarchy of this object looking for @p name.
   *
   * @return An owning copy of the found prototype, or a null `object` if
   *         not found.  The underlying registry handle is non-owning, so
   *         `bison_add_ref` is called internally to produce a safe owner.
   */
  object find_class(bison_hash name) const {
    bison_handle found = bison_find_class(h_, name);
    if (!found)
      return object{};
    // bison_find_class returns a non-owning handle; add_ref to own it.
    return object(bison_add_ref(found));
  }

  // ── Scalar setters ────────────────────────────────────────────────────────

  /** @throws std::runtime_error on error. */
  void set_int(bison_hash name, int32_t value) {
    detail::check(bison_set_int(h_, name, value), "bison_set_int");
  }

  /** @throws std::runtime_error on error. */
  void set_float(bison_hash name, float value) {
    detail::check(bison_set_float(h_, name, value), "bison_set_float");
  }

  /** @throws std::runtime_error on error. */
  void set_bool(bison_hash name, bool value) {
    detail::check(bison_set_bool(h_, name, value ? 1 : 0), "bison_set_bool");
  }

  /** @throws std::runtime_error on error. */
  void set_string(bison_hash name, const char* value) {
    detail::check(bison_set_string(h_, name, value), "bison_set_string");
  }

  /** @throws std::runtime_error on error. */
  void set_string(bison_hash name, const std::string& value) {
    detail::check(
        bison_set_string(h_, name, value.c_str()), "bison_set_string");
  }

  /**
   * @brief Set a nested object field.
   *
   * The library increments @p value's ref-count so both this object and the
   * field share the same underlying instance.
   *
   * @throws std::runtime_error on error.
   */
  void set_object(bison_hash name, const object& value) {
    detail::check(bison_set_object(h_, name, value.h_), "bison_set_object");
  }

  /** @throws std::runtime_error on error. */
  void set_int_at(size_t index, int32_t value) {
    detail::check(bison_set_int_at(h_, index, value), "bison_set_int_at");
  }

  /** @throws std::runtime_error on error. */
  void set_float_at(size_t index, float value) {
    detail::check(bison_set_float_at(h_, index, value), "bison_set_float_at");
  }

  /** @throws std::runtime_error on error. */
  void set_string_at(size_t index, const char* value) {
    detail::check(bison_set_string_at(h_, index, value), "bison_set_string_at");
  }

  // ── Scalar getters ────────────────────────────────────────────────────────

  /**
   * @brief Get an `int32_t` field by hash key.
   * @throws std::runtime_error on type mismatch or null handle.
   */
  int32_t get_int(bison_hash name) const {
    int32_t v = 0;
    detail::check(bison_get_int(h_, name, &v), "bison_get_int");
    return v;
  }

  /**
   * @brief Get a `float` field by hash key.
   * @throws std::runtime_error on type mismatch or null handle.
   */
  float get_float(bison_hash name) const {
    float v = 0.0f;
    detail::check(bison_get_float(h_, name, &v), "bison_get_float");
    return v;
  }

  /**
   * @brief Get a `bool` field by hash key.
   * @throws std::runtime_error on type mismatch or null handle.
   */
  bool get_bool(bison_hash name) const {
    int v = 0;
    detail::check(bison_get_bool(h_, name, &v), "bison_get_bool");
    return v != 0;
  }

  /**
   * @brief Get a string field by hash key as `std::string`.
   * @throws std::runtime_error on type mismatch or null handle.
   */
  std::string get_string(bison_hash name) const {
    size_t len = 0;
    detail::check(
        bison_get_string(h_, name, nullptr, 0, &len),
        "bison_get_string (length query)");
    std::string result(len, '\0');
    detail::check(
        bison_get_string(h_, name, result.data(), len + 1, nullptr),
        "bison_get_string");
    return result;
  }

  /**
   * @brief Get a nested object field by hash key.
   *
   * @return An owning handle.  May be null if the field holds a null
   *         dynamic reference; check with `operator bool()`.
   * @throws std::runtime_error on type mismatch or null handle.
   */
  object get_object(bison_hash name) const {
    bison_handle out = nullptr;
    detail::check(bison_get_object(h_, name, &out), "bison_get_object");
    return object(out); // null dynamic ref is a valid result
  }

  /**
   * @brief Get an `int32_t` field by numeric index.
   * @throws std::runtime_error on type mismatch or null handle.
   */
  int32_t get_int_at(size_t index) const {
    int32_t v = 0;
    detail::check(bison_get_int_at(h_, index, &v), "bison_get_int_at");
    return v;
  }

  /**
   * @brief Get a `float` field by numeric index.
   * @throws std::runtime_error on type mismatch or null handle.
   */
  float get_float_at(size_t index) const {
    float v = 0.0f;
    detail::check(bison_get_float_at(h_, index, &v), "bison_get_float_at");
    return v;
  }

  /**
   * @brief Get a string field by numeric index as `std::string`.
   * @throws std::runtime_error on type mismatch or null handle.
   */
  std::string get_string_at(size_t index) const {
    size_t len = 0;
    detail::check(
        bison_get_string_at(h_, index, nullptr, 0, &len),
        "bison_get_string_at (length query)");
    std::string result(len, '\0');
    detail::check(
        bison_get_string_at(h_, index, result.data(), len + 1, nullptr),
        "bison_get_string_at");
    return result;
  }

  /** @brief Return the number of array-like (numeric-key) elements. */
  size_t size() const noexcept {
    return bison_size(h_);
  }

  // ── Methods ───────────────────────────────────────────────────────────────

  /**
   * @brief Register a C callback as a named method on this object.
   *
   * @param name  Method name hash (use `key()`).
   * @param fn    Callback function.
   * @param user  Arbitrary context pointer passed to @p fn on each call.
   * @throws std::runtime_error on duplicate or null error.
   */
  void add_method(bison_hash name, bison_method_fn fn, void* user = nullptr) {
    detail::check(bison_add_method(h_, name, fn, user), "bison_add_method");
  }

  /**
   * @brief Invoke a named method on this object.
   *
   * @param name    Method name hash (use `key()`).
   * @param params  Call arguments object.
   * @return Result object (caller owns).
   * @throws std::runtime_error if the method is not found or on error.
   */
  object call(bison_hash name, const object& params) const {
    bison_handle result = nullptr;
    detail::check(bison_call(h_, name, params.h_, &result), "bison_call");
    return object(result);
  }

 private:
  bison_handle h_ = nullptr;
  explicit object(bison_handle h) noexcept : h_(h) {}
};

// ────────────────────────────────────────────────────────────────────────────
// rmi_transport — RAII wrapper for bison_rmi_transport (in-memory)
// ────────────────────────────────────────────────────────────────────────────

/**
 * @brief RAII owner of a `bison_rmi_transport` (in-memory server transport).
 *
 * Move-only.  The transport must outlive any `rmi_server` or `rmi_client`
 * created from it.
 */
class rmi_transport {
 public:
  /**
   * @brief Create a new in-memory server transport.
   * @throws std::runtime_error on allocation failure.
   */
  rmi_transport() : t_(bison_rmi_transport_create()) {
    if (!t_)
      throw std::runtime_error("bison_rmi_transport_create failed");
  }

  ~rmi_transport() {
    bison_rmi_transport_destroy(t_);
  }

  rmi_transport(const rmi_transport&) = delete;
  rmi_transport& operator=(const rmi_transport&) = delete;

  rmi_transport(rmi_transport&& o) noexcept : t_(o.t_) {
    o.t_ = nullptr;
  }

  rmi_transport& operator=(rmi_transport&& o) noexcept {
    if (this != &o) {
      bison_rmi_transport_destroy(t_);
      t_ = o.t_;
      o.t_ = nullptr;
    }
    return *this;
  }

  /** @brief Return the raw handle without transferring ownership. */
  bison_rmi_transport get() const noexcept {
    return t_;
  }

 private:
  bison_rmi_transport t_ = nullptr;
};

// Forward declaration needed by rmi_proxy.
class rmi_client;

// ────────────────────────────────────────────────────────────────────────────
// rmi_proxy — RAII wrapper for bison_rmi_proxy (in-memory)
// ────────────────────────────────────────────────────────────────────────────

/**
 * @brief RAII owner of a `bison_rmi_proxy` (in-memory transport proxy).
 *
 * Move-only.  Destroyed via `bison_rmi_client_destroy_proxy`; the
 * `rmi_client` that created this proxy must outlive it.
 *
 * Proxy operations (`clear`, `set`, `get`, `call`, `call_oneway`) are
 * defined on this class directly because the in-memory C API uses the
 * proxy handle as the single locus for those calls.
 */
class rmi_proxy {
 public:
  /** @brief Construct a null (invalid) proxy. */
  rmi_proxy() noexcept = default;

  /** @brief Destructor: calls `bison_rmi_client_destroy_proxy`. */
  ~rmi_proxy();

  rmi_proxy(const rmi_proxy&) = delete;
  rmi_proxy& operator=(const rmi_proxy&) = delete;

  rmi_proxy(rmi_proxy&& o) noexcept : proxy_(o.proxy_), client_(o.client_) {
    o.proxy_ = nullptr;
    o.client_ = nullptr;
  }

  rmi_proxy& operator=(rmi_proxy&& o) noexcept;

  /** @brief `true` if the proxy is non-null. */
  explicit operator bool() const noexcept {
    return proxy_ != nullptr;
  }

  /** @brief Return the raw handle without transferring ownership. */
  bison_rmi_proxy get() const noexcept {
    return proxy_;
  }

  // ── Proxy operations ──────────────────────────────────────────────────────

  /**
   * @brief Clear all explicitly set fields on the remote object.
   * @throws std::runtime_error on failure.
   */
  void clear() {
    detail::check(
        static_cast<bison_error>(bison_rmi_proxy_clear(proxy_)),
        "bison_rmi_proxy_clear");
  }

  /**
   * @brief Apply a partial field update to the remote object.
   *
   * @param fields  Object containing the fields to apply.
   * @throws std::runtime_error on failure.
   */
  void set(const object& fields) {
    detail::check(
        static_cast<bison_error>(bison_rmi_proxy_set(proxy_, fields.get())),
        "bison_rmi_proxy_set");
  }

  /**
   * @brief Retrieve fields from the remote object.
   *
   * @param projection  Optional projection template; pass a default-constructed
   *                    (null) `object` for a full snapshot.
   * @return Owning result object.
   * @throws std::runtime_error on failure.
   *
   * @note `bison_rmi_proxy_get` uses an in-out parameter and releases the
   *       input handle internally before writing the result.  This wrapper
   *       calls `bison_add_ref` on the projection beforehand so the caller's
   *       `object` remains valid after the call.
   */
  object get(const object& projection = object{}) const {
    // The C function releases *fields_out before writing the result, so we
    // add_ref the projection to avoid affecting the caller's object.
    bison_handle inout = bison_add_ref(projection.get());
    bison_error err =
        static_cast<bison_error>(bison_rmi_proxy_get(proxy_, &inout));
    if (err != BISON_OK) {
      bison_release(inout);
      detail::check(err, "bison_rmi_proxy_get");
    }
    return object::own(inout);
  }

  /**
   * @brief Call a method on the remote object (blocking).
   *
   * @param method_name  Hashed method name (use `object::key()`).
   * @param params       Call arguments; pass a default-constructed `object`
   *                     for no arguments.
   * @return Result object (caller owns).
   * @throws std::runtime_error on failure.
   */
  object call(bison_hash method_name, const object& params = object{}) const {
    // bison_rmi_proxy_call requires a non-null params handle.
    object actual_params = params ? params : object::create(0);
    bison_handle result = nullptr;
    detail::check(
        static_cast<bison_error>(bison_rmi_proxy_call(
            proxy_, method_name, actual_params.get(), &result)),
        "bison_rmi_proxy_call");
    return object::own(result);
  }

  /**
   * @brief Call a method on the remote object without waiting for a response.
   *
   * @param method_name  Hashed method name (use `object::key()`).
   * @param params       Call arguments; pass a default-constructed `object`
   *                     for no arguments.
   * @throws std::runtime_error on failure.
   */
  void call_oneway(bison_hash method_name, const object& params = object{}) {
    object actual_params = params ? params : object::create(0);
    detail::check(
        static_cast<bison_error>(bison_rmi_proxy_call_oneway(
            proxy_, method_name, actual_params.get())),
        "bison_rmi_proxy_call_oneway");
  }

  /**
   * @brief Register a handler for a server-initiated event.
   *
   * @param event_name  Hashed event name (use `object::key()`).
   * @param handler     Callback invoked on the client's worker thread.
   * @param user        Arbitrary context pointer passed to @p handler.
   * @throws std::runtime_error on failure.
   */
  void on_event(
      bison_hash event_name,
      bison_rmi_event_fn handler,
      void* user = nullptr) {
    detail::check(
        static_cast<bison_error>(
            bison_rmi_proxy_on_event(proxy_, event_name, handler, user)),
        "bison_rmi_proxy_on_event");
  }

 private:
  friend class rmi_client;

  bison_rmi_proxy proxy_ = nullptr;
  bison_rmi_client client_ = nullptr; // non-owning; needed for destroy_proxy

  rmi_proxy(bison_rmi_proxy proxy, bison_rmi_client client) noexcept
      : proxy_(proxy), client_(client) {}
};

// ────────────────────────────────────────────────────────────────────────────
// rmi_client — RAII wrapper for bison_rmi_client (in-memory)
// ────────────────────────────────────────────────────────────────────────────

/**
 * @brief RAII owner of a `bison_rmi_client` (in-memory transport client).
 *
 * Move-only.  The owning `rmi_transport` must outlive this object.
 * The destructor calls `bison_rmi_client_disconnect` then
 * `bison_rmi_client_destroy`.
 */
class rmi_client {
 public:
  /**
   * @brief Create a client connected to @p transport.
   *
   * @throws std::runtime_error on allocation failure.
   */
  explicit rmi_client(const rmi_transport& transport)
      : c_(bison_rmi_client_create(transport.get())) {
    if (!c_)
      throw std::runtime_error("bison_rmi_client_create failed");
  }

  ~rmi_client() {
    if (c_) {
      bison_rmi_client_disconnect(c_);
      bison_rmi_client_destroy(c_);
    }
  }

  rmi_client(const rmi_client&) = delete;
  rmi_client& operator=(const rmi_client&) = delete;

  rmi_client(rmi_client&& o) noexcept : c_(o.c_) {
    o.c_ = nullptr;
  }

  rmi_client& operator=(rmi_client&& o) noexcept {
    if (this != &o) {
      if (c_) {
        bison_rmi_client_disconnect(c_);
        bison_rmi_client_destroy(c_);
      }
      c_ = o.c_;
      o.c_ = nullptr;
    }
    return *this;
  }

  /** @brief Connect to the server and start the worker thread. */
  void connect() {
    detail::check(
        static_cast<bison_error>(bison_rmi_client_connect(c_)),
        "bison_rmi_client_connect");
  }

  /** @brief Gracefully disconnect and join the worker thread. */
  void disconnect() {
    detail::check(
        static_cast<bison_error>(bison_rmi_client_disconnect(c_)),
        "bison_rmi_client_disconnect");
  }

  /**
   * @brief Query the server for class metadata.
   *
   * @param klass  Hashed class name (use `object::key()`); `0` for all.
   * @return Owning descriptor object.
   * @throws std::runtime_error on failure.
   */
  object describe(bison_hash klass = 0) const {
    bison_handle h = bison_rmi_client_describe(c_, klass);
    if (!h)
      throw std::runtime_error("bison_rmi_client_describe failed");
    return object::own(h);
  }

  /**
   * @brief Instantiate a remote object on the server.
   *
   * @param klass   Hashed class name (use `object::key()`).
   * @param params  Construction parameters; pass a default-constructed
   *                `object` for none.
   * @return Owning proxy for the remote object.
   * @throws std::runtime_error on failure.
   */
  rmi_proxy instantiate(bison_hash klass, const object& params = object{}) {
    bison_rmi_proxy p = bison_rmi_client_instantiate(c_, klass, params.get());
    if (!p)
      throw std::runtime_error("bison_rmi_client_instantiate failed");
    return rmi_proxy(p, c_);
  }

  /** @brief Return the raw handle without transferring ownership. */
  bison_rmi_client get() const noexcept {
    return c_;
  }

 private:
  bison_rmi_client c_ = nullptr;
};

// ── rmi_proxy out-of-line definitions ─────────────────────────────────────

inline rmi_proxy::~rmi_proxy() {
  if (proxy_ && client_)
    bison_rmi_client_destroy_proxy(client_, proxy_);
}

inline rmi_proxy& rmi_proxy::operator=(rmi_proxy&& o) noexcept {
  if (this != &o) {
    if (proxy_ && client_)
      bison_rmi_client_destroy_proxy(client_, proxy_);
    proxy_ = o.proxy_;
    client_ = o.client_;
    o.proxy_ = nullptr;
    o.client_ = nullptr;
  }
  return *this;
}

// ────────────────────────────────────────────────────────────────────────────
// rmi_server — RAII wrapper for bison_rmi_server (in-memory)
// ────────────────────────────────────────────────────────────────────────────

/**
 * @brief RAII owner of a `bison_rmi_server` (in-memory transport server).
 *
 * Move-only.  The owning `rmi_transport` must outlive this object.  The
 * destructor calls `bison_rmi_server_stop` followed by
 * `bison_rmi_server_destroy`.
 */
class rmi_server {
 public:
  /**
   * @brief Create a server backed by @p transport.
   *
   * @throws std::runtime_error on allocation failure.
   */
  explicit rmi_server(const rmi_transport& transport)
      : srv_(bison_rmi_server_create(transport.get())) {
    if (!srv_)
      throw std::runtime_error("bison_rmi_server_create failed");
  }

  ~rmi_server() {
    if (srv_) {
      bison_rmi_server_stop(srv_);
      bison_rmi_server_destroy(srv_);
    }
  }

  rmi_server(const rmi_server&) = delete;
  rmi_server& operator=(const rmi_server&) = delete;

  rmi_server(rmi_server&& o) noexcept : srv_(o.srv_) {
    o.srv_ = nullptr;
  }

  rmi_server& operator=(rmi_server&& o) noexcept {
    if (this != &o) {
      if (srv_) {
        bison_rmi_server_stop(srv_);
        bison_rmi_server_destroy(srv_);
      }
      srv_ = o.srv_;
      o.srv_ = nullptr;
    }
    return *this;
  }

  /**
   * @brief Start the server's accept loop.
   * @throws std::runtime_error on failure.
   */
  void listen() {
    detail::check(bison_rmi_server_listen(srv_), "bison_rmi_server_listen");
  }

  /**
   * @brief Stop the server, close all connections, join worker threads.
   *
   * Idempotent — safe to call multiple times or before destruction.
   */
  void stop() noexcept {
    if (srv_)
      bison_rmi_server_stop(srv_);
  }

  /** @brief Return the raw handle without transferring ownership. */
  bison_rmi_server get() const noexcept {
    return srv_;
  }

 private:
  bison_rmi_server srv_ = nullptr;
};

} // namespace bdg::bison_c
