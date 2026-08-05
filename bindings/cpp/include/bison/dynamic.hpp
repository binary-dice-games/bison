// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file dynamic.hpp
 * @brief Header-only C++ wrapper around `bison_c.h`, giving the precompiled
 *        `bison_abi` shared library an interface that mirrors the internal
 *        `bdg::bison::dynamic` C++ API (`src/bison/bison_object.hpp`) as
 *        closely as the C ABI allows.
 *
 * ## Why this exists
 *
 * `bison_c.h` exposes Bison as a stable C ABI so it can be consumed from a
 * precompiled `bison_abi.{so,dylib,dll}` without rebuilding against the
 * internal C++ headers (no C++ ABI/ODR compatibility required between the
 * library and the consumer — the same reason the Python and C# bindings
 * exist, see `docs/bindings.md`). This header gives C++ consumers of that
 * same precompiled binary an ergonomic, RAII, exception-throwing API
 * instead of hand-written `bison_handle`/error-code plumbing, while staying
 * *header-only*: including this file and linking `bison_abi` is all a
 * project needs, exactly like `#include "bison_c.h"` itself.
 *
 * ## How closely this mirrors the internal API
 *
 * Same class name (`dynamic`), same `"name"_key` compile-time hashing, the
 * same `operator[]` / `addField()` / `addMethod()` / `call()` /
 * `addClass()` / `instantiate()` vocabulary. Code written against
 * `src/bison/bison.hpp` and code written against this header look the same
 * at most call sites, modulo the `bdg::bison::abi` namespace below.
 *
 * ### Why `bdg::bison::abi` and not `bdg::bison`
 *
 * This binding lives one namespace level deeper than the internal API
 * (`bdg::bison::abi::dynamic`, not `bdg::bison::dynamic`) for a concrete,
 * tested reason, not just caution: `bison_abi`'s shared library only applies
 * `BISON_API` visibility to the C functions declared in `bison_c.h` /
 * `rmi_c.h` — every internal C++ symbol it links in from `libbison.a`
 * (`bison_c.cpp` calls straight into `bdg::bison::dynamic`) keeps GCC/Clang's
 * *default* (exported) visibility, because nothing in this project's build
 * passes `-fvisibility=hidden`. Reusing the exact name `bdg::bison::dynamic`
 * for this wrapper — a completely different, incompatible class layout —
 * would therefore mangle to the *same* symbol names
 * (`_ZN3bdg5bison7dynamic4sizeEv` and friends) that `libbison_abi.so`
 * already exports as weak/COMDAT symbols. A consumer executable that also
 * defines those symbols (from including this header) triggers ELF symbol
 * interposition: calls made *inside* `bison_abi.so`'s own internal C++ code
 * get silently rebound, at load time, to this header's differently-laid-out
 * class instead of the library's own — corrupting memory the moment any
 * internal method (e.g. `bison_size()` calling the real
 * `dynamic::size()`) runs. This was reproduced directly while developing
 * this binding (a segfault inside `std::__shared_ptr::get()`, several
 * frames into what looked like infinite mutual recursion between the two
 * unrelated `size()` implementations) before the namespace was pushed one
 * level deeper to make the mangled names disjoint. `key_t`, `hash()`, and
 * every other symbol in this binding are namespaced under `abi` for the
 * same reason, even though most of them (plain `constexpr` functions with
 * no persistent object identity) are lower-risk than `dynamic` itself.
 *
 * A few further differences are unavoidable because the C ABI's surface is
 * a strict subset of the internal API:
 * - **No raw buffer/stream serialization.** `bison_c.h` only exposes
 *   `to_json()` / `to_yaml()` / `pretty()` (matching the Python/C# bindings)
 *   — there is no ABI entry point for `serialize(buffer_serializer&)` or the
 *   schema-driven wire format.
 * - **No generic field enumeration.** There is no ABI call to iterate an
 *   object's named fields, so `addMethod()`'s callback receives `result` as
 *   an out-parameter to populate in place (mirroring `bison_method_fn`
 *   itself) rather than returning a `dynamic` by value the way the internal
 *   `method_fn` does — the ABI has no way to copy an arbitrary field set
 *   from a fresh returned object into the caller-owned `result` handle.
 * - **Indexed (numeric) field access is `int32_t`/`float`/`std::string`
 *   only** — `bison_c.h` only exports `bison_{get,set}_{int,float,string}_at`,
 *   not `_at` variants for `bool`/`key_t`/nested objects.
 * - **Vector-typed fields can be registered but not read back** —
 *   `bison_add_field_vector_*` exists for initial registration, but there is
 *   no `bison_get_vector_*` to read the value back through the ABI.
 * - **Copy semantics intentionally match the internal `dynamic`'s quirky
 *   asymmetric design**: the copy constructor performs a deep clone
 *   (`bison_clone`, mirroring `dynamic(const dynamic&)`'s `clone_ptr()`
 *   recursion), while copy-assignment is deleted (mirroring
 *   `dynamic::operator=(const dynamic&) = delete`). Move is cheap pointer
 *   transfer either way.
 */

#pragma once

#include "bison_c.h"

#include "attributes.hpp"
#include "exception.hpp"
#include "key.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bdg::bison::abi {

class dynamic;
class field_ref;

/**
 * @brief Callable type for methods attached to a `dynamic` object.
 *
 * Unlike the internal `bdg::bison::method_fn` (`dynamic(dynamic&, const
 * dynamic&)`, returning the result by value), this callback populates
 * @p result in place — the same self/params/result triple `bison_method_fn`
 * itself uses. See this header's top-level doc comment for why: the ABI has
 * no way to copy an arbitrary field set out of a freshly-returned `dynamic`
 * into the library-owned `result` handle.
 *
 * @param self    The object the method is being called on (mutable, and a
 *                *non-owning* `dynamic` view — do not let it outlive the call).
 * @param params  Read-only call arguments (non-owning view).
 * @param result  Output object to populate (non-owning view); the library
 *                owns it, so it must not be released.
 */
using method_fn = std::function<void(dynamic& self, const dynamic& params, dynamic& result)>;

// ═══════════════════════════════════════════════════════════════════════════
// dynamic
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief RAII, reference-counted handle to a Bison dynamic object, backed by
 *        `bison_handle` (`bison_c.h`).
 *
 * Field access mirrors the internal `dynamic::operator[]`: `obj["score"_key]
 * = 42;`, `int32_t v = obj["score"_key].as<int32_t>();`. See `field_ref`.
 */
class dynamic {
 public:
  static constexpr hash_t CLASS = hash("__class"); /**< Reserved: class name hash. */
  static constexpr hash_t PARENT = hash("__parent"); /**< Reserved: parent class hash. */
  static constexpr hash_t NAMESPACE = hash("__namespace"); /**< Reserved: namespace hash (0 = global). */

  /**
   * @brief Construct a new, empty dynamic object.
   * @param klass  Hash of the class name; `key_t{0U}` for an anonymous object.
   */
  explicit dynamic(key_t klass = key_t{0U}) : handle_(bison_create(klass)), owned_(true) {
    if (!handle_)
      throw std::bad_alloc();
  }

  /**
   * @brief Deep-copy @p other via `bison_clone()`.
   *
   * Mirrors the internal `dynamic(const dynamic&)`, which recursively clones
   * nested `dynamic_ptr` fields rather than aliasing them.
   */
  dynamic(const dynamic& other) : handle_(bison_clone(other.handle_)), owned_(true) {
    if (!handle_)
      throw std::bad_alloc();
  }

  /** @brief Deleted, mirroring `dynamic::operator=(const dynamic&) = delete`.
   *  Use `.clone()` to obtain an explicit deep copy. */
  dynamic& operator=(const dynamic&) = delete;

  dynamic(dynamic&& other) noexcept : handle_(other.handle_), owned_(other.owned_) {
    other.handle_ = nullptr;
    other.owned_ = false;
  }

  dynamic& operator=(dynamic&& other) noexcept {
    if (this != &other) {
      release();
      handle_ = other.handle_;
      owned_ = other.owned_;
      other.handle_ = nullptr;
      other.owned_ = false;
    }
    return *this;
  }

  ~dynamic() {
    release();
  }

  // ── Raw-handle interop ────────────────────────────────────────────────

  /**
   * @brief Take ownership of an existing `bison_handle` (e.g. one returned
   *        by a `bison_*` call made directly against `bison_c.h`).
   * @param h  Handle to adopt; released by this wrapper's destructor.
   */
  static dynamic adopt(bison_handle h) {
    return dynamic(h, true);
  }

  /**
   * @brief Wrap an existing `bison_handle` without taking ownership.
   *
   * The returned wrapper never calls `bison_release()`; @p h must outlive
   * it. Used for the `self`/`params`/`result` views passed into
   * `method_fn` callbacks, and available for advanced direct-ABI interop.
   * @param h  Handle to view; may be `nullptr`.
   */
  static dynamic borrow(bison_handle h) {
    return dynamic(h, false);
  }

  /** @brief The underlying `bison_handle`, for direct `bison_c.h` calls. */
  bison_handle native_handle() const {
    return handle_;
  }

  /** @brief Relinquish ownership and return the raw handle without releasing
   *         it. `*this` becomes a moved-from (invalid) instance. */
  bison_handle release_handle() {
    bison_handle h = handle_;
    handle_ = nullptr;
    owned_ = false;
    return h;
  }

  /** @brief True unless this instance has been moved from. */
  bool valid() const {
    return handle_ != nullptr;
  }

  // ── Lifecycle helpers ────────────────────────────────────────────────

  /** @brief Return an independent deep copy (via `bison_clone()`). Same as
   *         the copy constructor, spelled out for call sites that want to be
   *         explicit about cloning. */
  dynamic clone() const {
    return adopt(checked(bison_clone(handle_), "clone"));
  }

  /** @brief Return a new `dynamic` sharing the same underlying object
   *         (via `bison_add_ref()`); mutations through either are visible
   *         to both. */
  dynamic add_ref() const {
    return adopt(checked(bison_add_ref(handle_), "add_ref"));
  }

  // ── Field access ─────────────────────────────────────────────────────

  /** @brief Access or create the field named @p name. See `field_ref`. */
  field_ref operator[](key_t name);
  /** @copydoc operator[](key_t) */
  field_ref operator[](key_t name) const;

  /** @brief Access or create the array-like field at numeric index @p pos.
   *  Only `int32_t`/`float`/`std::string` are supported at an index (ABI
   *  limitation — see this header's top-level doc comment). */
  field_ref operator[](size_t pos);
  /** @copydoc operator[](size_t) */
  field_ref operator[](size_t pos) const;

  /**
   * @brief Read field @p name as type @p T. Equivalent to
   *        `(*this)[name].as<T>()`.
   * @tparam T  One of `int32_t`, `float`, `bool`, `std::string`, `key_t`,
   *            `dynamic`.
   */
  template <typename T>
  T as(key_t name) const;

  /** @brief Number of array-like (numeric-index) elements. */
  size_t size() const {
    return bison_size(handle_);
  }

  /** @brief True if `size() == 0`. Note this only reflects array-like
   *         elements, matching `bison_size()` — not named fields. */
  bool empty() const {
    return size() == 0;
  }

  // ── Methods ──────────────────────────────────────────────────────────

  /**
   * @brief Register a callable as a named method on this object.
   * @param name  Method name.
   * @param fn    Implementation; see `method_fn`'s doc comment for its
   *              self/params/result signature.
   * @param meta  Optional attribute annotations.
   */
  void addMethod(key_t name, method_fn fn, attributes meta = {});

  /**
   * @brief Invoke a named method on this object.
   * @param name    Method to call.
   * @param params  Read-only call arguments.
   * @return The method's result, owned by the caller.
   */
  dynamic call(key_t name, const dynamic& params);

  /** @brief `call()` with an empty (freshly constructed) params object. */
  dynamic call(key_t name) {
    return call(name, dynamic());
  }

  // ── Field registration with optional attribute metadata ────────────────
  //
  // Unlike `operator[]=`, these fail (return `false`) if the field already
  // exists, mirroring the internal `dynamic::addField()` / the Python and
  // C# bindings' `add_field()` / `AddField()`.

  bool addField(key_t name, int32_t value, attributes meta = {});
  bool addField(key_t name, float value, attributes meta = {});
  bool addField(key_t name, bool value, attributes meta = {});
  bool addField(key_t name, const char* value, attributes meta = {});
  bool addField(key_t name, const std::string& value, attributes meta = {}) {
    return addField(name, value.c_str(), std::move(meta));
  }

  /**
   * @brief Declare a new `bison::key_t`-valued field — the `addField()`
   *        counterpart to `field_ref::operator=(key_t)`, for the same
   *        reason a bare `addField(name, int32_t)` can't dispatch to this.
   */
  bool addFieldKey(key_t name, key_t value, attributes meta = {});

  /** @brief Declare a `std::vector<bool>` field. Read-back is not supported
   *  through this ABI (see this header's top-level doc comment). */
  bool addField(key_t name, const std::vector<bool>& values, attributes meta = {});
  /** @brief Declare a `std::vector<int32_t>` field. Read-back is not
   *  supported through this ABI. */
  bool addField(key_t name, const std::vector<int32_t>& values, attributes meta = {});
  /** @brief Declare a `std::vector<float>` field. Read-back is not
   *  supported through this ABI. */
  bool addField(key_t name, const std::vector<float>& values, attributes meta = {});
  /** @brief Declare a `std::vector<uint8_t>` (raw byte buffer) field.
   *  Read-back is not supported through this ABI. */
  bool addField(key_t name, const std::vector<uint8_t>& values, attributes meta = {});

  // ── Field / method attributes ───────────────────────────────────────────

  attributes field_attributes(key_t name) const;
  attributes method_attributes(key_t name) const;

  // ── Serialization ────────────────────────────────────────────────────

  /** @brief Serialize to a JSON string. Pass `indent = -1` for compact
   *         output. Field keys appear as `"#<decimal>"` (no name map is
   *         available through the C ABI). */
  std::string to_json(int indent = 2) const;

  /** @brief Serialize to a YAML string (block style). */
  std::string to_yaml() const;

  /** @brief Human-readable representation, via `bison_print()`. */
  std::string pretty(bool multiline = true, const std::string& indent = "  ") const;

  // ── Class registry (static) ─────────────────────────────────────────────

  /** @brief Parse a JSON string and return the root object. */
  static dynamic from_json(const std::string& text);
  /** @brief Parse a YAML string and return the root object. */
  static dynamic from_yaml(const std::string& text);

  /**
   * @brief Register @p klass as a class prototype in namespace @p ns.
   * @return `true` on success, `false` if a class with the same name is
   *         already registered in @p ns.
   */
  static bool addClass(key_t ns, const dynamic& klass, key_t parent = key_t{0U}, attributes meta = {});
  /** @brief `addClass()` in the global namespace. */
  static bool addClass(const dynamic& klass, key_t parent = key_t{0U}, attributes meta = {}) {
    return addClass(key_t{0U}, klass, parent, std::move(meta));
  }

  /**
   * @brief Look up a registered class prototype.
   * @return An owning view of the prototype (this wrapper takes its own
   *         reference via `bison_add_ref()`), or `std::nullopt` if not found.
   */
  static std::optional<dynamic> find_class(key_t ns, key_t klass);
  /** @brief `find_class()` in the global namespace. */
  static std::optional<dynamic> find_class(key_t klass) {
    return find_class(key_t{0U}, klass);
  }

  /** @brief Create a new instance of a registered class (with inherited
   *         fields and methods) in namespace @p ns. */
  static dynamic instantiate(key_t ns, key_t klass);
  /** @brief `instantiate()` in the global namespace. */
  static dynamic instantiate(key_t klass) {
    return instantiate(key_t{0U}, klass);
  }

  /** @brief Remove all registered classes from the global registry. */
  static void clear_registry() {
    bison_clear_registry();
  }

  /** @brief Read the class-level attribute metadata of a registered class. */
  static attributes class_attributes(key_t ns, key_t klass);
  /** @brief `class_attributes()` in the global namespace. */
  static attributes class_attributes(key_t klass) {
    return class_attributes(key_t{0U}, klass);
  }

 private:
  friend class field_ref;

  dynamic(bison_handle h, bool owned) : handle_(h), owned_(owned) {}

  void release() {
    if (owned_ && handle_)
      bison_release(handle_);
    handle_ = nullptr;
    owned_ = false;
  }

  static bison_handle checked(bison_handle h, const char* context) {
    if (!h)
      throw std::runtime_error(std::string(context) + ": operation failed");
    return h;
  }

  bison_handle handle_;
  bool owned_;
};

// ═══════════════════════════════════════════════════════════════════════════
// field_ref — the proxy returned by dynamic::operator[]
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Proxy returned by `dynamic::operator[]`, standing in for the
 *        internal `field&`.
 *
 * Supports typed assignment (`obj["x"_key] = 42;`) and typed reads
 * (`int32_t v = obj["x"_key].as<int32_t>();` or the implicit-conversion
 * form `int32_t v = obj["x"_key];`), matching the internal `field`'s
 * `operator=` / `as<T>()` / `operator T()` vocabulary. Which C ABI call an
 * assignment or read dispatches to is resolved entirely by C++ overload
 * resolution on the value's static type — exactly mirroring how the
 * internal `field_base` variant alternative is selected by the assigned
 * value's type.
 *
 * `field_ref` does not itself enforce constness beyond what the compiler
 * checks statically: the underlying `bison_handle` has no const-protection
 * at the ABI level, matching the internal `dynamic`'s own `mutable fields_`
 * (auto-vivification requires write access even from `operator[] const`).
 */
class field_ref {
 public:
  field_ref(bison_handle owner, hash_t key, bool is_index) : owner_(owner), key_(key), is_index_(is_index) {}

  // ── Assignment ───────────────────────────────────────────────────────

  field_ref& operator=(int32_t value) {
    detail::check(is_index_ ? bison_set_int_at(owner_, key_, value) : bison_set_int(owner_, key_, value), "set_int");
    return *this;
  }

  field_ref& operator=(float value) {
    detail::check(
        is_index_ ? bison_set_float_at(owner_, key_, value) : bison_set_float(owner_, key_, value), "set_float");
    return *this;
  }

  field_ref& operator=(double value) {
    return *this = static_cast<float>(value);
  }

  field_ref& operator=(bool value) {
    if (is_index_)
      throw std::logic_error("indexed fields do not support bool (bison_c.h has no bison_set_bool_at)");
    detail::check(bison_set_bool(owner_, key_, value ? 1 : 0), "set_bool");
    return *this;
  }

  field_ref& operator=(const char* value) {
    detail::check(
        is_index_ ? bison_set_string_at(owner_, key_, value) : bison_set_string(owner_, key_, value), "set_string");
    return *this;
  }

  field_ref& operator=(const std::string& value) {
    return *this = value.c_str();
  }

  /** @brief Set a `bison::key_t`-valued field, e.g. an object's `"id"`. A
   *  distinct overload (not the same as `operator=(int32_t)`) because
   *  `key_t` is its own field-variant alternative — see `key.hpp`. */
  field_ref& operator=(key_t value) {
    if (is_index_)
      throw std::logic_error("indexed fields do not support key_t (bison_c.h has no bison_set_key_at)");
    detail::check(bison_set_key(owner_, key_, value), "set_key");
    return *this;
  }

  /** @brief Set a nested `dynamic` object field (`bison_set_object()` takes
   *  its own reference, so @p value may be reused afterward). */
  field_ref& operator=(const dynamic& value);

  /** @brief Set a nested object field to null. */
  field_ref& operator=(std::nullptr_t) {
    if (is_index_)
      throw std::logic_error("indexed fields do not support nested objects (bison_c.h has no bison_set_object_at)");
    detail::check(bison_set_object(owner_, key_, nullptr), "set_object(null)");
    return *this;
  }

  // ── Reading ──────────────────────────────────────────────────────────

  /**
   * @brief Read the field's value as type @p T, throwing `bison_exception` on a
   *        type mismatch — matching the internal `field::as<T>()`.
   * @tparam T  One of `int32_t`, `float`, `bool`, `std::string`, `key_t`,
   *            `dynamic`.
   */
  template <typename T>
  T as() const {
    if constexpr (std::is_same_v<T, int32_t>) {
      int32_t v = 0;
      detail::check(is_index_ ? bison_get_int_at(owner_, key_, &v) : bison_get_int(owner_, key_, &v), "get_int");
      return v;
    } else if constexpr (std::is_same_v<T, float>) {
      float v = 0.f;
      detail::check(is_index_ ? bison_get_float_at(owner_, key_, &v) : bison_get_float(owner_, key_, &v), "get_float");
      return v;
    } else if constexpr (std::is_same_v<T, bool>) {
      if (is_index_)
        throw std::logic_error("indexed fields do not support bool (bison_c.h has no bison_get_bool_at)");
      int v = 0;
      detail::check(bison_get_bool(owner_, key_, &v), "get_bool");
      return v != 0;
    } else if constexpr (std::is_same_v<T, key_t>) {
      if (is_index_)
        throw std::logic_error("indexed fields do not support key_t (bison_c.h has no bison_get_key_at)");
      hash_t v = 0;
      detail::check(bison_get_key(owner_, key_, &v), "get_key");
      return key_t{v};
    } else if constexpr (std::is_same_v<T, std::string>) {
      size_t len = 0;
      auto probe = is_index_ ? bison_get_string_at(owner_, key_, nullptr, 0, &len)
                             : bison_get_string(owner_, key_, nullptr, 0, &len);
      detail::check(probe, "get_string");
      std::string result(len, '\0');
      if (len > 0) {
        detail::check(
            is_index_ ? bison_get_string_at(owner_, key_, result.data(), len + 1, nullptr)
                      : bison_get_string(owner_, key_, result.data(), len + 1, nullptr),
            "get_string");
      }
      return result;
    } else if constexpr (std::is_same_v<T, dynamic>) {
      auto obj = as_object();
      if (!obj)
        throw bison_exception(BISON_ERR_NULL, "field is a null object reference");
      return std::move(*obj);
    } else {
      static_assert(sizeof(T) == 0, "field_ref::as<T>: unsupported field type T");
    }
  }

  /** @brief Read a nested object field, or `std::nullopt` for an explicit
   *  null reference. Use this instead of `as<dynamic>()` when the field may
   *  legitimately be null. */
  std::optional<dynamic> as_object() const;

  operator dynamic() const {
    return as<dynamic>();
  }
  operator std::string() const {
    return as<std::string>();
  }
  template <typename T, std::enable_if_t<!std::is_same_v<T, dynamic> && !std::is_same_v<T, std::string>, int> = 0>
  operator T() const {
    return as<T>();
  }

 private:
  bison_handle owner_;
  hash_t key_;
  bool is_index_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Out-of-line definitions needing both classes complete
// ═══════════════════════════════════════════════════════════════════════════

inline field_ref dynamic::operator[](key_t name) {
  return field_ref(handle_, name, false);
}
inline field_ref dynamic::operator[](key_t name) const {
  return field_ref(handle_, name, false);
}
inline field_ref dynamic::operator[](size_t pos) {
  return field_ref(handle_, static_cast<hash_t>(pos), true);
}
inline field_ref dynamic::operator[](size_t pos) const {
  return field_ref(handle_, static_cast<hash_t>(pos), true);
}

template <typename T>
inline T dynamic::as(key_t name) const {
  return (*this)[name].template as<T>();
}

inline field_ref& field_ref::operator=(const dynamic& value) {
  if (is_index_)
    throw std::logic_error("indexed fields do not support nested objects (bison_c.h has no bison_set_object_at)");
  detail::check(bison_set_object(owner_, key_, value.native_handle()), "set_object");
  return *this;
}

inline std::optional<dynamic> field_ref::as_object() const {
  if (is_index_)
    throw std::logic_error("indexed fields do not support nested objects (bison_c.h has no bison_get_object_at)");
  bison_handle child = nullptr;
  detail::check(bison_get_object(owner_, key_, &child), "get_object");
  if (!child)
    return std::nullopt;
  return dynamic::adopt(child);
}

// ── Method registration ───────────────────────────────────────────────────

namespace detail {

// `bison_add_method()` only stores the raw C function pointer/`user` data it
// is given; the `method_fn` closure behind the trampoline below must outlive
// every call the library makes into it. Prototypes are normally registered
// once at startup and live for the process's lifetime (like the class
// registry itself), so — mirroring the Python binding's `_registered_prototypes`
// list (see `bindings/python/bison/dynamic.py`) — this registry simply keeps
// every registered closure alive for the process's lifetime rather than
// trying to tie its lifetime to a `dynamic` wrapper that may be destroyed
// while the underlying (add_ref'd or registry-owned) object is still live.
inline std::mutex& method_registry_mutex() {
  static std::mutex m;
  return m;
}

inline std::vector<std::shared_ptr<method_fn>>& method_registry() {
  static std::vector<std::shared_ptr<method_fn>> registry;
  return registry;
}

inline method_fn* register_method_fn(method_fn fn) {
  auto stored = std::make_shared<method_fn>(std::move(fn));
  std::lock_guard<std::mutex> lock(method_registry_mutex());
  method_registry().push_back(stored);
  return stored.get();
}

inline void method_trampoline(bison_handle self, bison_handle params, bison_handle result, void* user) {
  auto* fn = static_cast<method_fn*>(user);
  dynamic self_view = dynamic::borrow(self);
  dynamic params_view = dynamic::borrow(params);
  dynamic result_view = dynamic::borrow(result);
  try {
    (*fn)(self_view, params_view, result_view);
  } catch (...) {
    // C ABI boundary: exceptions must not propagate back into bison_abi.
  }
}

} // namespace detail

inline void dynamic::addMethod(key_t name, method_fn fn, attributes meta) {
  method_fn* stored = detail::register_method_fn(std::move(fn));
  ::bison_attributes c = meta.to_c();
  detail::check(
      bison_add_method(handle_, name, &detail::method_trampoline, stored, meta.empty() ? nullptr : &c), "add_method");
}

inline dynamic dynamic::call(key_t name, const dynamic& params) {
  bison_handle result = nullptr;
  detail::check(bison_call(handle_, name, params.handle_, &result), "call");
  return dynamic::adopt(result);
}

// ── Field registration ────────────────────────────────────────────────────

inline bool dynamic::addField(key_t name, int32_t value, attributes meta) {
  ::bison_attributes c = meta.to_c();
  auto rc = bison_add_field_int(handle_, name, value, meta.empty() ? nullptr : &c);
  if (rc == BISON_ERR_DUPLICATE)
    return false;
  detail::check(rc, "add_field_int");
  return true;
}

inline bool dynamic::addField(key_t name, float value, attributes meta) {
  ::bison_attributes c = meta.to_c();
  auto rc = bison_add_field_float(handle_, name, value, meta.empty() ? nullptr : &c);
  if (rc == BISON_ERR_DUPLICATE)
    return false;
  detail::check(rc, "add_field_float");
  return true;
}

inline bool dynamic::addField(key_t name, bool value, attributes meta) {
  ::bison_attributes c = meta.to_c();
  auto rc = bison_add_field_bool(handle_, name, value ? 1 : 0, meta.empty() ? nullptr : &c);
  if (rc == BISON_ERR_DUPLICATE)
    return false;
  detail::check(rc, "add_field_bool");
  return true;
}

inline bool dynamic::addField(key_t name, const char* value, attributes meta) {
  ::bison_attributes c = meta.to_c();
  auto rc = bison_add_field_string(handle_, name, value, meta.empty() ? nullptr : &c);
  if (rc == BISON_ERR_DUPLICATE)
    return false;
  detail::check(rc, "add_field_string");
  return true;
}

inline bool dynamic::addFieldKey(key_t name, key_t value, attributes meta) {
  ::bison_attributes c = meta.to_c();
  auto rc = bison_add_field_key(handle_, name, value, meta.empty() ? nullptr : &c);
  if (rc == BISON_ERR_DUPLICATE)
    return false;
  detail::check(rc, "add_field_key");
  return true;
}

inline bool dynamic::addField(key_t name, const std::vector<bool>& values, attributes meta) {
  std::vector<int> ints(values.begin(), values.end()); // std::vector<bool> is bit-packed; bison_c.h wants int*
  ::bison_attributes c = meta.to_c();
  auto rc = bison_add_field_vector_bool(
      handle_, name, ints.empty() ? nullptr : ints.data(), ints.size(), meta.empty() ? nullptr : &c);
  if (rc == BISON_ERR_DUPLICATE)
    return false;
  detail::check(rc, "add_field_vector_bool");
  return true;
}

inline bool dynamic::addField(key_t name, const std::vector<int32_t>& values, attributes meta) {
  ::bison_attributes c = meta.to_c();
  auto rc = bison_add_field_vector_int(
      handle_, name, values.empty() ? nullptr : values.data(), values.size(), meta.empty() ? nullptr : &c);
  if (rc == BISON_ERR_DUPLICATE)
    return false;
  detail::check(rc, "add_field_vector_int");
  return true;
}

inline bool dynamic::addField(key_t name, const std::vector<float>& values, attributes meta) {
  ::bison_attributes c = meta.to_c();
  auto rc = bison_add_field_vector_float(
      handle_, name, values.empty() ? nullptr : values.data(), values.size(), meta.empty() ? nullptr : &c);
  if (rc == BISON_ERR_DUPLICATE)
    return false;
  detail::check(rc, "add_field_vector_float");
  return true;
}

inline bool dynamic::addField(key_t name, const std::vector<uint8_t>& values, attributes meta) {
  ::bison_attributes c = meta.to_c();
  auto rc = bison_add_field_vector_bytes(
      handle_, name, values.empty() ? nullptr : values.data(), values.size(), meta.empty() ? nullptr : &c);
  if (rc == BISON_ERR_DUPLICATE)
    return false;
  detail::check(rc, "add_field_vector_bytes");
  return true;
}

// ── Attributes ─────────────────────────────────────────────────────────

inline attributes dynamic::field_attributes(key_t name) const {
  ::bison_attributes c{};
  detail::check(bison_get_field_attributes(handle_, name, &c), "field_attributes");
  return attributes::from_c(c);
}

inline attributes dynamic::method_attributes(key_t name) const {
  ::bison_attributes c{};
  detail::check(bison_get_method_attributes(handle_, name, &c), "method_attributes");
  return attributes::from_c(c);
}

// ── Serialization ──────────────────────────────────────────────────────

inline std::string dynamic::to_json(int indent) const {
  char* out = nullptr;
  detail::check(bison_to_json(handle_, indent, &out), "to_json");
  std::string result(out ? out : "");
  bison_free_string(out);
  return result;
}

inline std::string dynamic::to_yaml() const {
  char* out = nullptr;
  detail::check(bison_to_yaml(handle_, &out), "to_yaml");
  std::string result(out ? out : "");
  bison_free_string(out);
  return result;
}

inline std::string dynamic::pretty(bool multiline, const std::string& indent) const {
  ::bison_print_options opts{};
  opts.multiline = multiline ? 1 : 0;
  opts.indent = indent.c_str();
  char* out = nullptr;
  detail::check(bison_print(handle_, &opts, &out), "pretty");
  std::string result(out ? out : "");
  bison_free_string(out);
  return result;
}

// ── Class registry ────────────────────────────────────────────────────

inline dynamic dynamic::from_json(const std::string& text) {
  bison_handle h = bison_from_json(text.c_str());
  if (!h)
    throw bison_exception(BISON_ERR_PARSE, "from_json");
  return dynamic::adopt(h);
}

inline dynamic dynamic::from_yaml(const std::string& text) {
  bison_handle h = bison_from_yaml(text.c_str());
  if (!h)
    throw bison_exception(BISON_ERR_PARSE, "from_yaml");
  return dynamic::adopt(h);
}

inline bool dynamic::addClass(key_t ns, const dynamic& klass, key_t parent, attributes meta) {
  ::bison_attributes c = meta.to_c();
  auto rc = bison_add_class(ns, klass.handle_, parent, meta.empty() ? nullptr : &c);
  if (rc == BISON_ERR_DUPLICATE)
    return false;
  detail::check(rc, "add_class");
  return true;
}

inline std::optional<dynamic> dynamic::find_class(key_t ns, key_t klass) {
  bison_handle h = bison_find_class(ns, klass);
  if (!h)
    return std::nullopt;
  return dynamic::adopt(checked(bison_add_ref(h), "find_class"));
}

inline dynamic dynamic::instantiate(key_t ns, key_t klass) {
  bison_handle h = bison_instantiate(ns, klass);
  if (!h)
    throw std::bad_alloc();
  return dynamic::adopt(h);
}

inline attributes dynamic::class_attributes(key_t ns, key_t klass) {
  ::bison_attributes c{};
  detail::check(bison_get_class_attributes(ns, klass, &c), "class_attributes");
  return attributes::from_c(c);
}

} // namespace bdg::bison::abi
