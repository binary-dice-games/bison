// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file bison_common.hpp
 * @brief Fundamental types, hashing utilities, and forward declarations shared
 *        across all Bison core headers.
 *
 * This header establishes the `bdg::bison` namespace and defines:
 * - Endianness detection and the `byte_swap` helper.
 * - The FNV-1a-based `hash_t` / `key_t` name-hashing system.
 * - The `dynamic_ptr`, `method`, `field_base`, and `collection` type aliases
 *   used throughout the object model.
 *
 * All other core headers include this file via their own `#pragma once` guard.
 */

#pragma once

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace bdg::bison {

class buffer_serializer;
class buffer_deserializer;
class stream_serializer;
class stream_deserializer;
class attribute;
class field;
class dynamic;

/** @brief Endianness constants and a compile-time native-endian probe. */
namespace endian {
const size_t big = 1; /**< Big-endian sentinel value. */
const size_t little = 0; /**< Little-endian sentinel value. */
/** @brief Runtime-detected native byte order; equals `little` or `big`. */
const size_t native = []() {
  uint32_t i = 0x01020304;
  return ((char*)&i)[0] == 0x04 ? little : big;
}();
} // namespace endian

/**
 * @brief Swap the byte order of a scalar value on little-endian hosts.
 *
 * On big-endian hosts this is a no-op.  Compiler intrinsics are used where
 * available (`__builtin_bswap*` on GCC/Clang).  `__builtin_bit_cast` is used
 * rather than `std::bit_cast` so the header also builds on GCC 10 (e.g. the
 * devtoolset-10 toolchain in older manylinux images), whose libstdc++ ships
 * `<bit>` but not `std::bit_cast` (added in GCC 11).
 *
 * @tparam T  A trivially-copyable type of size 1, 2, 4, or 8 bytes.
 * @param  value  The value to byte-swap.
 * @return The byte-swapped value, or @p value unchanged on big-endian hosts.
 */
template <typename T>
constexpr T byte_swap(T value) {
  if constexpr (sizeof(T) == 1)
    return value;
  if (endian::native == endian::big)
    return value;
#if defined(__GNUC__) || defined(__clang__)
  if constexpr (sizeof(T) == 2) {
    uint16_t u = __builtin_bit_cast(uint16_t, value);
    u = __builtin_bswap16(u);
    return __builtin_bit_cast(T, u);
  } else if constexpr (sizeof(T) == 4) {
    uint32_t u = __builtin_bit_cast(uint32_t, value);
    u = __builtin_bswap32(u);
    return __builtin_bit_cast(T, u);
  } else if constexpr (sizeof(T) == 8) {
    uint64_t u = __builtin_bit_cast(uint64_t, value);
    u = __builtin_bswap64(u);
    return __builtin_bit_cast(T, u);
  }
#endif

  T result{};
  const size_t size = sizeof(T);
  for (size_t i = 0; i < size; ++i) {
    reinterpret_cast<unsigned char*>(&result)[size - i - 1] = reinterpret_cast<const unsigned char*>(&value)[i];
  }
  return result;
}

/** @brief 32-bit hash type used as the wire representation of a name. */
using hash_t = uint32_t;
/** @brief Byte buffer used for in-memory binary payloads. */
using buffer = std::vector<uint8_t>;

/**
 * @brief Compute a compile-time FNV-1a hash of a null-terminated string.
 *
 * The MSB of the result is always set so that hashed names are distinguishable
 * from plain numeric indices (which are small non-negative integers stored in
 * the same `key_t` map).
 *
 * @param input  Null-terminated ASCII/UTF-8 string to hash.
 * @return FNV-1a hash with the MSB forced to 1.
 */
constexpr hash_t hash(const char* input) {
  hash_t value = sizeof(hash_t) == 8 ? 0xcbf29ce484222325 : 0x811c9dc5;
  hash_t mask = sizeof(hash_t) == 8 ? 0x8000000000000000 : 0x80000000;
  const hash_t prime = sizeof(hash_t) == 8 ? 0x00000100000001b3 : 0x01000193;

  while (*input) {
    value ^= static_cast<hash_t>(*input);
    value *= prime;
    ++input;
  }

  return value | mask;
}

/**
 * @brief Key type that wraps a `hash_t` and serves as a map key.
 *
 * Supports construction from raw `hash_t`, `const char*`, and `std::string`
 * so that callers can use either pre-hashed values or plain strings
 * interchangeably.  Also satisfies the `Hash` and `KeyEqual` requirements for
 * use as both template arguments in `std::unordered_map`.
 */
struct _key_t {
  constexpr _key_t(hash_t v = 0) : id(v) {}
  constexpr _key_t(const char* input) : id(hash(input)) {}
  _key_t(const std::string& input) : id(hash(input.c_str())) {}
  constexpr operator hash_t() const {
    return id;
  }
  std::size_t operator()(const struct _key_t& k) const {
    return id;
  }
  bool operator()(const struct _key_t& lhs, const struct _key_t& rhs) const {
    return lhs.id == rhs.id;
  }
  hash_t id;
};

/** @brief Convenience alias for `_key_t`. */
using key_t = struct _key_t;

/**
 * @brief User-defined literal that hashes a string constant to a `key_t`.
 *
 * Enables concise compile-time field-name hashing, e.g. `"score"_key`.
 * Returns `key_t` (not `hash_t`) so that `obj["name"_key]` resolves to the
 * `operator[](key_t)` overload via an exact match, instead of the numeric
 * `operator[](size_t)` overload via an integral widening conversion.
 *
 * @param name  Null-terminated string literal.
 * @param size  Length of the literal (unused; provided by the compiler).
 * @return `key_t` wrapping the FNV-1a hash of @p name with MSB set.
 */
constexpr key_t operator""_key(const char* name, std::size_t size) noexcept {
  return key_t{hash(name)};
}

// Forward-declared here so `_rkey` can call it; see full doc below.
void register_key_name(hash_t h, std::string_view name);

/**
 * @brief Look up a name registered via `_rkey` / `register_key_name`.
 *
 * Reads the raw registry directly: the exact literal string a key was
 * registered under (e.g. recovering "TextEditor" from its hash). Never
 * overridden by a `DisplayName` attribute -- `DisplayName` is a separate
 * mechanism for reflection-tool UI labels (see `bison_object.hpp`) and is
 * not reversible, so it is never mixed into name recovery.
 *
 * @param h  Hash produced by `hash()`, `_key`, or `_rkey`.
 * @return The exact string last registered for @p h via `register_key_name`/
 *         `_rkey`, or `std::nullopt` if @p h was never registered that way.
 */
std::optional<std::string> lookup_registered_key_name(hash_t h);

/**
 * @brief Registering key literal — hashes @p name like `_key` and also records
 *        the string in the global key-name registry.
 *
 * Use this literal only at registration sites -- `addClass`, `addMethod`,
 * `addField`, and method input/output spec field keys -- where a name needs
 * to be recoverable for trace/print output later:
 * @code{.cpp}
 *   addMethod("preset"_rkey, method{...});
 *   addField("label"_rkey, field{...});
 * @endcode
 * The registered string becomes available via `lookup_registered_key_name()`,
 * and `print()` consults it automatically as a fallback for any field or
 * method key without a `DisplayName` attribute.
 *
 * This literal is **not** `constexpr`: it takes a registry lock and writes
 * to a process-wide map on every evaluation, so it is far more costly than
 * `_key`. Do not use it for ordinary field access (`obj["a"_rkey]`) --
 * use plain `_key` there, and reserve `_rkey` for the one-time registration
 * call sites above.
 *
 * @param name  Null-terminated string literal.
 * @param size  Length of the literal (unused; provided by the compiler).
 * @return `key_t` wrapping the FNV-1a hash of @p name with MSB set.
 */
inline key_t operator""_rkey(const char* name, std::size_t /*size*/) noexcept {
  register_key_name(hash(name), name);
  return key_t{hash(name)};
}

/**
 * @brief Reference-counted smart pointer to a `dynamic` object.
 *
 * Extends `std::shared_ptr<dynamic>` with additional constructors that
 * allow in-place construction of a `dynamic` and conversion from a bare
 * `std::shared_ptr<dynamic>`, plus `operator[]` for direct field access
 * without the `(*ptr)[key]` boilerplate.
 */
class dynamic_ptr : public std::shared_ptr<dynamic> {
 public:
  using std::shared_ptr<dynamic>::shared_ptr;
  using std::shared_ptr<dynamic>::operator=;

  dynamic_ptr(const std::shared_ptr<dynamic>& that);
  dynamic_ptr(std::shared_ptr<dynamic>&& that);

  dynamic_ptr& operator=(const std::shared_ptr<dynamic>& that);
  dynamic_ptr& operator=(std::shared_ptr<dynamic>&& that);

  dynamic_ptr(dynamic&& that);
  // Split from a single defaulted-argument constructor: MSVC requires
  // `field` complete to reason about a `std::map<key_t, field>` default
  // argument, but this header only forward-declares `field` (it is fully
  // defined in bison_object.hpp). Two non-defaulted overloads avoid ever
  // needing `field` complete here.
  dynamic_ptr(key_t klass = 0U);
  dynamic_ptr(key_t klass, std::map<key_t, field>&& fields);

  /// @brief Field access: `ptr[key] = value` without needing `(*ptr)[key]`.
  template <typename K>
  decltype(auto) operator[](K key) const {
    return (**this)[key];
  }
};

/**
 * @brief Callable type for methods attached to a `dynamic` object.
 *
 * Methods receive a mutable reference to the object they are called on
 * (`self`) and a read-only `dynamic` containing call arguments (`params`),
 * and return a `dynamic` result.
 */
using method_fn = std::function<dynamic(dynamic& /*self*/, const dynamic& /*params*/)>;

/**
 * @brief Variant base that enumerates all value types a `field` can hold.
 *
 * The index of each alternative in this variant determines its one-byte
 * serialisation tag.  The ordering must not be changed without a
 * corresponding wire-format version bump.
 */
using field_base = std::variant<
    std::monostate, /**< Empty / null field. */
    hash_t, /**< Raw 32-bit hash value. */
    key_t, /**< Hashed name key. */
    bool, /**< Boolean value. */
    int32_t, /**< 32-bit signed integer. */
    float, /**< Single-precision float. */
    dynamic_ptr, /**< Nested dynamic object (may be null). */
    std::string, /**< UTF-8 string. */
    std::vector<bool>, /**< Homogeneous bool array. */
    std::vector<int32_t>, /**< Homogeneous int32 array. */
    std::vector<float>, /**< Homogeneous float array. */
    std::vector<uint8_t> /**< Raw byte array. */
    >;

/**
 * @brief Unordered map of `key_t` → `dynamic_ptr` used for the class
 *        registry and other named object collections.
 */
using collection = std::unordered_map<key_t, dynamic_ptr, key_t, key_t>;

/**
 * @brief The namespace registry: a map of namespace hash → class collection.
 *
 * The global (default) namespace uses key `0U`.  Named namespaces use the
 * FNV-1a hash of their name (produced by `hash()` or `"name"_key`).
 * Each entry holds an independent `collection` of class prototypes so that
 * the same class name may be registered in multiple namespaces without
 * collision.
 */
using namespace_map = std::unordered_map<key_t, collection, key_t, key_t>;

/**
 * @brief Register a human-readable name for a hash key.
 *
 * Stores @p name in the global key-name registry, readable back via
 * `lookup_registered_key_name()`.  Thread-safe; may be called from any
 * thread.
 *
 * Use the `_rkey` literal (e.g. `(void)"descriptor"_rkey`) as a concise way
 * to register a name at startup without an explicit call here.
 *
 * @param h     Hash produced by `hash()` or `_key` / `_rkey` literal.
 * @param name  Human-readable name to associate with @p h.
 */
// (declared above, before `_rkey`, so that the literal can call it)

} // namespace bdg::bison
