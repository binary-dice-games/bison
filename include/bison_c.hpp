// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file bison_c.hpp
 * @brief C++ RAII wrappers for the Bison C ABI (`bison_c.h`).
 *
 * Provides `bdg::bison::abi::dynamic` — a thin, header-only RAII class that
 * sits top of the stable C ABI and offer a more idiomatic C++ interface:
 *
 * - Handles are released automatically in destructors.
 * - Copying a `dynamic` calls `bison_add_ref()` so reference counting is
 *   transparent to the caller.
 * - Scalar getters return values directly and throw `std::runtime_error` on
 *   failure instead of using out-parameters and error codes.
 * - String getters return `std::string`.
 * - Boolean fields use `bool` rather than `int`.
 * - Method registration uses `std::function`, supporting lambdas with captures.
 *
 * Because every call goes through the C ABI, these wrappers are safe to use
 * from a different DLL or shared library — no C++ vtables, `std::string`
 * instances, or allocator state are shared with the bison shared library.
 *
 * ### Example
 * @code{.cpp}
 * #include "bison_c.hpp"
 * using namespace bdg::bison::abi;
 *
 * auto score_obj = dynamic::create(dynamic::key("Player"));
 * score_obj.set(dynamic::key("score"), 42)
 *          .set(dynamic::key("name"), "Alice");
 * int32_t score = score_obj.get<int32_t>(dynamic::key("score"));  // 42
 *
 * // Register a method with a lambda:
 * score_obj.add_method(
 *     dynamic::key("reset"),
 *     [](dynamic& self, const dynamic& params, dynamic& result) {
 *   // Method implementation
 *     });
 * @endcode
 */

#pragma once

#include "bison_c.h"

#include <functional>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>

namespace bdg::bison::abi {

class dynamic;
namespace detail {

inline void check(bison_error err, const char* msg) {
  if (err != BISON_OK) {
    throw std::runtime_error(
        std::string(msg) + " (code " + std::to_string(static_cast<int>(err)) +
        ")");
  }
}

constexpr bison_hash hash_compute(const char* name, size_t size) {
  // 32-bit FNV-1a with the MSB forced to 1 to match bison_key semantics.
  bison_hash value = 0x811c9dc5u;
  for (size_t i = 0; i < size; ++i) {
    value ^= static_cast<unsigned char>(name[i]);
    value *= 0x01000193u;
  }
  return value | 0x80000000u;
}

template <size_t N>
consteval bison_hash hash_literal(const char (&name)[N]) {
  return hash_compute(name, N - 1);
}

using MethodCallback =
    std::function<void(dynamic& self, const dynamic& params, dynamic& result)>;

inline void adapter_wrapper(
    bison_handle self,
    bison_handle params,
    bison_handle result,
    void* user);

} // namespace detail

constexpr bison_hash operator""_key(
    const char* name,
    std::size_t size) noexcept {
  return detail::hash_compute(name, size);
}

// ────────────────────────────────────────────────────────────────────────────
// dynamic — RAII wrapper for bison_handle

/**
 * @brief RAII owner of a `bison_handle`.
 *
 * Automatically releases the underlying handle in the destructor.  Copying
 * calls `bison_add_ref()` so both the original and the copy are independent
 * owners.  Moving leaves the source in the null (invalid) state.
 *
 * The default constructor produces a null handle; check validity with
 * `operator bool()` before calling any getter or setter.
 */
class dynamic {
 public:
  // ── Factories ─────────────────────────────────────────────────────────────

  /**
   * @brief Take ownership of an already-created raw handle.
   *
   * The caller must **not** call `bison_release()` on @p h after this.
   */
  static dynamic own(bison_handle h) noexcept {
    return dynamic(h);
  }

  /**
   * @brief Create an owning copy of a non-owning handle via `bison_add_ref`.
   *
   * Useful for handles returned by functions that do not transfer ownership,
   * such as `bison_find_class`.
   */
  static dynamic borrow(bison_handle h) noexcept {
    return dynamic(bison_add_ref(h));
  }

  /**
   * @brief Create a new, empty dynamic object.
   *
   * @param klass_name  Hashed class name (use `key()`); pass `0` for an
   *                    anonymous object.
   * @throws std::runtime_error on allocation failure.
   */
  static dynamic create(bison_hash klass_name = 0) {
    bison_handle h = bison_create(klass_name);
    if (!h)
      throw std::runtime_error("bison_create failed");
    return dynamic(h);
  }

  /**
   * @brief Instantiate a class-typed object in the global namespace.
   *
   * @param klass_name  Hashed class name (use `key()`).
   * @throws std::runtime_error on failure.
   */
  static dynamic instantiate(bison_hash klass_name) {
    return instantiate(static_cast<bison_hash>(0L), klass_name);
  }

  /**
   * @brief Instantiate a class-typed object in a named namespace.
   *
   * @param ns_name     Hashed namespace name (use `key()`); `0` for global.
   * @param klass_name  Hashed class name (use `key()`).
   * @throws std::runtime_error on failure.
   */
  static dynamic instantiate(bison_hash ns_name, bison_hash klass_name) {
    bison_handle h = bison_instantiate(ns_name, klass_name);
    if (!h)
      throw std::runtime_error("bison_instantiate failed");
    return dynamic(h);
  }

  /**
   * @brief Parse a JSON string into an object.
   *
   * @param json  Null-terminated UTF-8 JSON string.
   * @throws std::runtime_error on parse failure.
   */
  static dynamic from_json(const char* json) {
    bison_handle h = bison_from_json(json);
    if (!h)
      throw std::runtime_error("bison_from_json: parse error");
    return dynamic(h);
  }

  /**
   * @brief Parse a YAML string into an object.
   *
   * @param yaml  Null-terminated UTF-8 YAML string.
   * @throws std::runtime_error on parse failure.
   */
  static dynamic from_yaml(const char* yaml) {
    bison_handle h = bison_from_yaml(yaml);
    if (!h)
      throw std::runtime_error("bison_from_yaml: parse error");
    return dynamic(h);
  }

  // ── Lifecycle ─────────────────────────────────────────────────────────────

  /** @brief Construct a null (invalid) handle. */
  dynamic() noexcept = default;

  ~dynamic() {
    reset();
  }

  /** @brief Copy: increments the reference count via `bison_add_ref`. */
  dynamic(const dynamic& o) noexcept : h_(bison_add_ref(o.h_)) {}

  /** @brief Move: takes the handle; source becomes null. */
  dynamic(dynamic&& o) noexcept : h_(o.h_) {
    o.h_ = nullptr;
  }

  dynamic& operator=(const dynamic& o) noexcept {
    if (this != &o) {
      reset();
      h_ = bison_add_ref(o.h_);
    }
    return *this;
  }

  dynamic& operator=(dynamic&& o) noexcept {
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
  template <size_t N>
  static consteval bison_hash key(const char (&name)[N]) {
    return detail::hash_literal(name);
  }

  /** @brief Compute the FNV-1a hash at runtime for dynamic strings. */
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
  static void add_class(bison_hash parent_name, const dynamic& klass) {
    add_class(static_cast<bison_hash>(0L), klass, parent_name);
  }

  /**
   * @brief Register @p klass as a prototype in a named namespace.
   *
   * @param ns_name      Hashed namespace name (use `key()`); `0` for global.
   * @param klass        Object whose `__class` field has already been set.
   * @param parent_name  Hashed parent class name; `0` for a root class.
   * @throws std::runtime_error on null handle or duplicate class name.
   */
  static void
  add_class(bison_hash ns_name, const dynamic& klass, bison_hash parent_name) {
    detail::check(
        bison_add_class(ns_name, klass.h_, parent_name), "bison_add_class");
  }

  /**
   * @brief Look up a class in the global namespace.
   *
   * @param klass_name  Hashed class name (use `key()`).
   * @return An owning copy of the found prototype, or a null `dynamic` if
   *         not found.
   */
  static dynamic find_class(bison_hash klass_name) {
    return find_class(static_cast<bison_hash>(0L), klass_name);
  }

  /**
   * @brief Look up a class in a specific namespace.
   *
   * @param ns_name     Hashed namespace name (use `key()`); `0` for global.
   * @param klass_name  Hashed class name (use `key()`).
   * @return An owning copy of the found prototype, or a null `dynamic` if
   *         not found.
   */
  static dynamic find_class(bison_hash ns_name, bison_hash klass_name) {
    bison_handle found = bison_find_class(ns_name, klass_name);
    if (!found)
      return dynamic{};
    // bison_find_class_ns returns a non-owning handle; add_ref to own it.
    return dynamic(bison_add_ref(found));
  }

  // ── Scalar setters ────────────────────────────────────────────────────────
  // Overloaded set methods with method chaining support.

  /** @brief Set an `int32_t` field. @throws std::runtime_error on error. */
  dynamic& set(bison_hash name, int32_t value) {
    detail::check(bison_set_int(h_, name, value), "bison_set_int");
    return *this;
  }

  /** @brief Set a `float` field. @throws std::runtime_error on error. */
  dynamic& set(bison_hash name, float value) {
    detail::check(bison_set_float(h_, name, value), "bison_set_float");
    return *this;
  }

  /** @brief Set a `bool` field. @throws std::runtime_error on error. */
  dynamic& set(bison_hash name, bool value) {
    detail::check(bison_set_bool(h_, name, value ? 1 : 0), "bison_set_bool");
    return *this;
  }

  /** @brief Set a string field from C string. @throws std::runtime_error on
   * error. */
  dynamic& set(bison_hash name, const char* value) {
    detail::check(bison_set_string(h_, name, value), "bison_set_string");
    return *this;
  }

  /** @brief Set a string field from std::string. @throws std::runtime_error on
   * error. */
  dynamic& set(bison_hash name, const std::string& value) {
    detail::check(
        bison_set_string(h_, name, value.c_str()), "bison_set_string");
    return *this;
  }

  /** @brief Set a nested object field. @throws std::runtime_error on error. */
  dynamic& set(bison_hash name, const dynamic& value) {
    detail::check(bison_set_object(h_, name, value.h_), "bison_set_object");
    return *this;
  }

  /** @brief Set an `int32_t` field by index. @throws std::runtime_error on
   * error. */
  dynamic& set(size_t index, int32_t value) {
    detail::check(bison_set_int_at(h_, index, value), "bison_set_int_at");
    return *this;
  }

  /** @brief Set a `float` field by index. @throws std::runtime_error on error.
   */
  dynamic& set(size_t index, float value) {
    detail::check(bison_set_float_at(h_, index, value), "bison_set_float_at");
    return *this;
  }

  /** @brief Set a string field by index. @throws std::runtime_error on error.
   */
  dynamic& set(size_t index, const char* value) {
    detail::check(bison_set_string_at(h_, index, value), "bison_set_string_at");
    return *this;
  }

  // ── Scalar getters ────────────────────────────────────────────────────────
  // Template-based getters for type-safe field retrieval by name.

  /**
   * @brief Get a field by hash key with automatic type deduction.
   *
   * Supports both named field access (by `bison_hash`) and indexed/array
   * access (by `size_t index`). The overload is selected automatically based
   * on the parameter type.
   *
   * @throws std::runtime_error on type mismatch or null handle.
   * @code
   *   // Named field access:
   *   int32_t x = obj.get<int32_t>(key("x"));
   *   float y = obj.get<float>(key("y"));
   *   bool z = obj.get<bool>(key("z"));
   *   std::string s = obj.get<std::string>(key("s"));
   *   dynamic child = obj.get<dynamic>(key("child"));
   *
   *   // Indexed/array access:
   *   int32_t elem0 = obj.get<int32_t>(0);
   *   float elem1 = obj.get<float>(1);
   *   std::string elem2 = obj.get<std::string>(2);
   * @endcode
   */
  template <typename T>
  T get(bison_hash name) const;

  // Explicit specializations for supported types
  template <>
  int32_t get<int32_t>(bison_hash name) const {
    int32_t v = 0;
    detail::check(bison_get_int(h_, name, &v), "bison_get_int");
    return v;
  }

  template <>
  float get<float>(bison_hash name) const {
    float v = 0.0f;
    detail::check(bison_get_float(h_, name, &v), "bison_get_float");
    return v;
  }

  template <>
  bool get<bool>(bison_hash name) const {
    int v = 0;
    detail::check(bison_get_bool(h_, name, &v), "bison_get_bool");
    return v != 0;
  }

  template <>
  std::string get<std::string>(bison_hash name) const {
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

  template <>
  dynamic get<dynamic>(bison_hash name) const {
    bison_handle out = nullptr;
    detail::check(bison_get_object(h_, name, &out), "bison_get_object");
    return dynamic(out); // null dynamic ref is a valid result
  }

  // Template-based indexed getters for array-like access (overloads of get<T>)
  template <typename T>
  T get(size_t index) const;

  template <>
  int32_t get<int32_t>(size_t index) const {
    int32_t v = 0;
    detail::check(bison_get_int_at(h_, index, &v), "bison_get_int_at");
    return v;
  }

  template <>
  float get<float>(size_t index) const {
    float v = 0.0f;
    detail::check(bison_get_float_at(h_, index, &v), "bison_get_float_at");
    return v;
  }

  template <>
  std::string get<std::string>(size_t index) const {
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
   * @brief Register a callable as a named method on this object.
   *
   * Accepts any callable (function pointer, lambda, functor, or
   * `std::function`) and registers it as a method. Lambdas with captures are
   * fully supported.
   *
   * @param name  Method name hash (use `key()`).
   * @param fn    Callable that takes `(dynamic& self, const dynamic& params,
   *              dynamic& result)` and returns void.
   * @throws std::runtime_error on duplicate or null error.
   * @code
   *   // With a lambda:
   *   obj.add_method(key("greet"),
   *                  [](dynamic& self, const dynamic& params, dynamic& result)
   * {
   *                    // Implementation
   *                  });
   *
   *   // With a std::function:
   *   std::function<void(dynamic&, const dynamic&, dynamic&)> fn =
   *       [](dynamic& self, const dynamic& params, dynamic& result) { ... };
   *   obj.add_method(key("speak"), fn);
   * @endcode
   */
  void add_method(
      bison_hash name,
      std::function<void(dynamic&, const dynamic&, dynamic&)> fn) {
    auto* callback = new detail::MethodCallback(std::move(fn));
    detail::check(
        bison_add_method(
            h_, name, detail::adapter_wrapper, callback,
            [](void* p) { delete static_cast<detail::MethodCallback*>(p); }),
        "bison_add_method");
  }

  /**
   * @brief Invoke a named method on this object.
   *
   * @param name    Method name hash (use `key()`).
   * @param params  Call arguments object.
   * @return Result object (caller owns).
   * @throws std::runtime_error if the method is not found or on error.
   */
  dynamic call(bison_hash name, const dynamic& params) const {
    bison_handle result = nullptr;
    detail::check(bison_call(h_, name, params.h_, &result), "bison_call");
    return dynamic(result);
  }

  // ── Field accessor proxy ──────────────────────────────────────────────────

  /**
   * @brief Proxy returned by `operator[]` that dispatches reads and writes.
   *
   * Assigning to a `field` calls the matching `set()` overload on the
   * parent `dynamic`; converting it to a concrete type calls the matching
   * `get<T>()` overload.  This enables natural bracket-access syntax:
   *
   * @code{.cpp}
   *   obj["name"_key] = "Alice";            // calls set(key, const char*)
   *   obj["score"_key] = 42;                // calls set(key, int32_t)
   *   obj["ratio"_key] = 0.5f;              // calls set(key, float)
   *   obj["active"_key] = true;             // calls set(key, bool)
   *   obj["child"_key] = other_dynamic;     // calls set(key, const dynamic&)
   *
   *   std::string name  = obj["name"_key];  // calls get<std::string>
   *   int32_t     score = obj["score"_key]; // calls get<int32_t>
   *   float       ratio = obj["ratio"_key]; // calls get<float>
   *   bool        ok    = obj["active"_key];// calls get<bool>
   *   dynamic     child = obj["child"_key]; // calls get<dynamic>
   *
   *   std::cout << obj["name"_key];         // streams via std::string
   * @endcode
   *
   * @note The proxy holds a **non-owning reference** to its parent `dynamic`.
   *       Do not store a `field` beyond the lifetime of its parent.
   */
  class field {
   public:
    field(dynamic& parent, bison_hash key) noexcept
        : parent_(parent), key_(key) {}

    // Prevent storage of temporaries accidentally.
    field(const field&) = delete;
    field& operator=(const field&) = delete;

    // ── Setters (assignment) ───────────────────────────────────────────────

    field& operator=(int32_t value) {
      parent_.set(key_, value);
      return *this;
    }

    field& operator=(float value) {
      parent_.set(key_, value);
      return *this;
    }

    field& operator=(bool value) {
      parent_.set(key_, value);
      return *this;
    }

    field& operator=(const char* value) {
      parent_.set(key_, value);
      return *this;
    }

    field& operator=(const std::string& value) {
      parent_.set(key_, value);
      return *this;
    }

    field& operator=(const dynamic& value) {
      parent_.set(key_, value);
      return *this;
    }

    // ── Getters (implicit conversions) ─────────────────────────────────────

    operator int32_t() const {
      return parent_.get<int32_t>(key_);
    }

    operator float() const {
      return parent_.get<float>(key_);
    }

    operator bool() const {
      return parent_.get<bool>(key_);
    }

    operator std::string() const {
      return parent_.get<std::string>(key_);
    }

    operator dynamic() const {
      return parent_.get<dynamic>(key_);
    }

    // ── Stream support ─────────────────────────────────────────────────────

    /** @brief Stream the field as a string (calls `get<std::string>`). */
    friend std::ostream& operator<<(std::ostream& os, const field& f) {
      return os << static_cast<std::string>(f);
    }

   private:
    dynamic& parent_;
    bison_hash key_;
  };

  /**
   * @brief Return a proxy that enables bracket-access reads and writes.
   *
   * @param name  Hashed field name (use the `""_key` literal operator).
   * @return A `field` proxy bound to this object and @p name.
   *
   * @code{.cpp}
   *   auto obj = dynamic::create("Player"_key);
   *   obj["name"_key]  = "John";
   *   obj["score"_key] = 100;
   *   std::cout << obj["name"_key];   // prints "John"
   * @endcode
   */
  field operator[](bison_hash name) {
    return field(*this, name);
  }

 private:
  bison_handle h_ = nullptr;

  explicit dynamic(bison_handle h) noexcept : h_(h) {}
};

inline void detail::adapter_wrapper(
    bison_handle self,
    bison_handle params,
    bison_handle result,
    void* user) {
  auto* callback = static_cast<MethodCallback*>(user);
  if (!callback) {
    return;
  }

  // Borrow to avoid taking ownership of temporary bridge handles created by
  // the C ABI layer while still presenting RAII objects to C++ callbacks.
  dynamic self_obj = dynamic::borrow(self);
  dynamic params_obj = dynamic::borrow(params);
  dynamic result_obj = dynamic::borrow(result);
  (*callback)(self_obj, params_obj, result_obj);
}

} // namespace bdg::bison::abi
