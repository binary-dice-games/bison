// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

#pragma once

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

/**
 * @file bison.hpp
 * @brief Bison — self-describing binary serialization and dynamic object
 * library.
 *
 * Bison provides a runtime object model where every object (`dynamic`) carries
 * named fields and callable methods. Objects can be serialized to a compact,
 * endian-safe binary stream and deserialized back without any external schema
 * file. The library is similar in concept to JSON (self-describing, nestable)
 * but uses a binary format for efficiency.
 *
 * Key capabilities:
 * - **Dynamic objects** with heterogeneous, named fields.
 * - **Method registration** — attach lambdas to objects and invoke them by
 *   name at runtime.
 * - **Class hierarchy** — register named classes with parent relationships;
 *   field and method lookup traverses the inheritance chain automatically.
 * - **Binary serialization** — endian-aware, size-prefixed, self-describing
 *   (standard mode) or schema-driven (template mode).
 * - **Field attributes** — attach typed metadata to individual fields.
 * - **JSON import** via the `extensions::from_json` helper.
 *
 * All public symbols live in the `bdg::bison` namespace.
 *
 * ### Minimal example
 * @code{.cpp}
 * #include <bison.hpp>
 * #include <sstream>
 * using namespace bdg::bison;
 *
 * dynamic obj{"MyClass"_key, {{"score"_key, int32_t{42}}}};
 * std::stringstream ss;
 * obj.serialize(stream_serializer(ss));
 * auto copy = dynamic::deserialize(stream_deserializer(ss));
 * @endcode
 */

/**
 * @namespace bdg::bison
 * @brief Root namespace for the Bison library.
 *
 * Contains all public types, free functions, and the `extensions`
 * sub-namespace.
 */
namespace bdg::bison {

class buffer_serializer;
class buffer_deserializer;
class stream_serializer;
class stream_deserializer;
class attribute;
class field;
class dynamic;

/**
 * @namespace bdg::bison::endian
 * @brief Compile-time and runtime endianness constants.
 *
 * Use these constants together with `byte_swap` to write portable
 * serialization code. `endian::native` is evaluated once at program start and
 * reflects the byte order of the current platform.
 */
namespace endian {
/** @brief Big-endian / network byte order (value = 1). This is the reference
 * byte order used on the wire. */
const size_t big = 1;
/** @brief Little-endian constant (value = 0). */
const size_t little = 0;
/** @brief Native byte order of the current platform, detected at startup. */
const size_t native = []() {
  uint32_t i = 0x01020304;
  return ((char*)&i)[0] == 0x04 ? little : big;
}();
} // namespace endian

/**
 * @brief Reverses the byte order of a scalar value when the platform is
 *        little-endian.
 *
 * Bison's wire format uses big-endian (network) byte order. On little-endian
 * platforms every multi-byte scalar must be swapped before writing and after
 * reading. On big-endian platforms the function is a no-op.
 *
 * Optimisation: on GCC / Clang the compiler built-ins
 * `__builtin_bswap16/32/64` are used for 2-, 4-, and 8-byte types; on MSVC
 * the equivalent `_byteswap_ushort/ulong/uint64` intrinsics are used.
 * Both sets are recognised by the compiler and lowered to a single `bswap`
 * instruction (x86) or `rev` instruction (ARM).  For all other platforms the
 * loop-based portable fallback is retained.
 *
 * @tparam T  An integral or floating-point type.
 * @param  value  The value whose bytes may be reversed.
 * @return The byte-swapped value (or the original value on big-endian hosts).
 */
template <typename T>
constexpr T byte_swap(T value) {
  if constexpr (sizeof(T) == 1)
    return value;
  if (endian::native == endian::big)
    return value;
#if defined(__GNUC__) || defined(__clang__)
  if constexpr (sizeof(T) == 2) {
    uint16_t u = std::bit_cast<uint16_t>(value);
    u = __builtin_bswap16(u);
    return std::bit_cast<T>(u);
  } else if constexpr (sizeof(T) == 4) {
    uint32_t u = std::bit_cast<uint32_t>(value);
    u = __builtin_bswap32(u);
    return std::bit_cast<T>(u);
  } else if constexpr (sizeof(T) == 8) {
    uint64_t u = std::bit_cast<uint64_t>(value);
    u = __builtin_bswap64(u);
    return std::bit_cast<T>(u);
  }
#elif defined(_MSC_VER)
  if constexpr (sizeof(T) == 2) {
    uint16_t u = std::bit_cast<uint16_t>(value);
    u = _byteswap_ushort(u);
    return std::bit_cast<T>(u);
  } else if constexpr (sizeof(T) == 4) {
    uint32_t u = std::bit_cast<uint32_t>(value);
    u = _byteswap_ulong(u);
    return std::bit_cast<T>(u);
  } else if constexpr (sizeof(T) == 8) {
    uint64_t u = std::bit_cast<uint64_t>(value);
    u = _byteswap_uint64(u);
    return std::bit_cast<T>(u);
  }
#endif

  // Portable fallback: byte-by-byte reversal.
  T result{};
  const size_t size = sizeof(T);
  for (size_t i = 0; i < size; ++i) {
    reinterpret_cast<unsigned char*>(&result)[size - i - 1] =
        reinterpret_cast<const unsigned char*>(&value)[i];
  }
  return result;
}

/** @brief Unsigned 32-bit integer used as a hashed field key. */
using hash_t = uint32_t;

/** @brief Canonical byte buffer type for serialized binary payloads. */
using buffer = std::vector<uint8_t>;

/**
 * @brief FNV-1a compile-time string hash.
 *
 * Produces a 32-bit hash of a null-terminated C string. The high bit is always
 * set so that the hash value is distinguishable from small numeric indices.
 *
 * @param input  Null-terminated string to hash.
 * @return 32-bit FNV-1a hash with the high bit set.
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
 * @brief User-defined string literal that hashes a field name at compile time.
 *
 * Use this literal to refer to named fields with zero runtime string-comparison
 * cost.
 *
 * @code{.cpp}
 * obj["velocity"_key] = 9.8f;
 * @endcode
 *
 * @param name  Pointer to the string literal characters.
 * @param size  Length of the string (provided by the compiler).
 * @return Compile-time `hash_t` for the given name.
 */
constexpr hash_t operator""_key(const char* name, std::size_t size) noexcept {
  return hash(name);
}

/**
 * @brief A hashable key type that wraps a 32-bit FNV-1a hash.
 *
 * `_key_t` can be constructed from a `hash_t`, a `const char*`, or a
 * `std::string`; in each case the stored value is the 32-bit hash of the
 * input string. The struct also satisfies the requirements of
 * `std::unordered_map`'s `Hash` and `KeyEqual` template parameters so that it
 * can be used directly as both the hasher and the comparator.
 *
 * Prefer the `"name"_key` literal to construct `key_t` values at compile time.
 */
struct _key_t {
  /** @brief Construct from a pre-computed hash value. */
  _key_t(hash_t v = 0) : id(v) {}
  /** @brief Construct by hashing a C string. */
  _key_t(const char* input) : id(hash(input)) {}
  /** @brief Construct by hashing a `std::string`. */
  _key_t(const std::string& input) : id(hash(input.c_str())) {}
  /** @brief Implicit conversion to the underlying `hash_t`. */
  operator hash_t() const {
    return id;
  }
  /** @brief Hash function for use as `std::unordered_map` hasher. */
  std::size_t operator()(const struct _key_t& k) const {
    return id;
  }
  /** @brief Equality predicate for use as `std::unordered_map` comparator. */
  bool operator()(const struct _key_t& lhs, const struct _key_t& rhs) const {
    return lhs.id == rhs.id;
  }
  /** @brief The stored 32-bit hash value. */
  hash_t id;
};

/**
 * @brief Alias for the hashable key struct.
 *
 * Use `key_t` (or the `"name"_key` literal) everywhere a field name is needed.
 */
using key_t = struct _key_t;

class field;
class dynamic;

class dynamic_ptr : public std::shared_ptr<dynamic> {
 public:
  using std::shared_ptr<dynamic>::shared_ptr;
  using std::shared_ptr<dynamic>::operator=;

  dynamic_ptr(const std::shared_ptr<dynamic>& that);
  dynamic_ptr(std::shared_ptr<dynamic>&& that);

  dynamic_ptr& operator=(const std::shared_ptr<dynamic>& that);
  dynamic_ptr& operator=(std::shared_ptr<dynamic>&& that);

  dynamic_ptr(dynamic&& that);
  dynamic_ptr(key_t klass = 0U, std::map<key_t, field>&& fields = {});
};

/**
 * @brief Callable type for methods attached to `dynamic` objects.
 *
 * A method receives the owning object as `self` (mutable reference) and a
 * `dynamic` holding the call arguments as `params` (const reference). It
 * returns a `dynamic` result object.
 *
 * @code{.cpp}
 * method greet = [](dynamic& self, const dynamic& params) -> dynamic {
 *     dynamic result;
 *     result["greeting"_key] = std::string{"hello"};
 *     return result;
 * };
 * obj.addMethod("greet"_key, greet);
 * dynamic out = obj.call("greet"_key, dynamic{});
 * @endcode
 */
using method =
    std::function<dynamic(dynamic& /*self*/, const dynamic& /*params*/)>;

/**
 * @brief The set of all types that a `field` may hold.
 *
 * This variant drives both runtime type checks and the binary serialization
 * type tag. The numeric index of each alternative in the variant is written
 * to the stream as a single byte when serializing.
 *
 * | Index | Type |
 * |-------|------|
 * | 0 | `std::monostate` (empty) |
 * | 1 | `hash_t` |
 * | 2 | `key_t` |
 * | 3 | `bool` |
 * | 4 | `int32_t` |
 * | 5 | `float` |
 * | 6 | `dynamic_ptr` |
 * | 7 | `std::string` |
 * | 8 | `std::vector<bool>` |
 * | 9 | `std::vector<int32_t>` |
 * | 10 | `std::vector<float>` |
 * | 11 | `std::vector<uint8_t>` |
 */
using field_base = std::variant<
    std::monostate,
    hash_t,
    key_t,
    bool,
    int32_t,
    float,
    dynamic_ptr,
    std::string,
    std::vector<bool>,
    std::vector<int32_t>,
    std::vector<float>,
    std::vector<uint8_t>>;

/**
 * @brief Map type used for the global class registry.
 *
 * Maps a class name (as a `key_t`) to its shared `dynamic` prototype.
 */
using collection = std::unordered_map<key_t, dynamic_ptr, key_t, key_t>;

/**
 * @brief RAII pointer proxy returned by `synchronized` lock operations.
 *
 * Holds the underlying lock for its entire lifetime. Provides pointer-like
 * access to the guarded datum. The proxy is move-only; it cannot be copied.
 *
 * When `isNull()` returns `true` the lock has been released early via
 * `unlock()` and dereferencing the proxy is undefined behaviour.
 *
 * @tparam T        Type of the guarded datum (may be `const`-qualified for
 *                  read-only proxies).
 * @tparam LockType Concrete lock type, e.g. `std::unique_lock<Mutex>` or
 *                  `std::shared_lock<Mutex>`.
 */
template <typename T, typename LockType>
class locked_ptr {
 public:
  /** @brief Construct a null (unlocked) proxy. */
  locked_ptr() = default;

  /** @brief Construct by taking ownership of both the data pointer and the
   *         pre-acquired lock. */
  locked_ptr(T* data, LockType lock) : data_(data), lock_(std::move(lock)) {}

  locked_ptr(const locked_ptr&) = delete;
  locked_ptr& operator=(const locked_ptr&) = delete;
  locked_ptr(locked_ptr&&) = default;
  locked_ptr& operator=(locked_ptr&&) = default;

  /** @brief Access the guarded object. Undefined behaviour when null. */
  T* operator->() const {
    return data_;
  }

  /** @brief Dereference the guarded object. Undefined behaviour when null. */
  T& operator*() const {
    return *data_;
  }

  /**
   * @brief Release the lock early and transition the proxy to the null state.
   *
   * After `unlock()` returns, `isNull()` is `true` and any dereference is
   * undefined behaviour.
   */
  void unlock() {
    lock_.unlock();
    data_ = nullptr;
  }

  /** @brief Return `true` when the proxy is in the null (unlocked) state. */
  bool isNull() const {
    return data_ == nullptr;
  }

  /** @brief Contextual boolean — `false` when null. */
  explicit operator bool() const {
    return data_ != nullptr;
  }

 private:
  T* data_ = nullptr;
  LockType lock_;
};

namespace detail {

/** @brief Concept satisfied by mutex types that support shared (read) locking,
 *         such as `std::shared_mutex`. */
template <typename Mutex>
concept shared_mutex_c = requires(Mutex& m) {
  m.lock_shared();
  m.unlock_shared();
};

} // namespace detail

/**
 * @brief Associates a datum with a mutex so that the lock must be explicitly
 *        acquired before the datum can be accessed.
 *
 * Inspired by `folly::Synchronized`. Instead of storing the mutex and the
 * data separately (where it is easy to forget to take the lock), `synchronized`
 * bundles them together and only exposes the data through a lock-holding
 * `locked_ptr` proxy.
 *
 * When `Mutex` supports shared locking (e.g. the default `std::shared_mutex`),
 * both `wlock()` (exclusive write) and `rlock()` (shared read) are available.
 * When only an exclusive mutex is used (e.g. `std::mutex`), only `wlock()` /
 * `lock()` are available.
 *
 * ### Example
 * @code{.cpp}
 * synchronized<std::vector<int>> vec;
 *
 * // Exclusive write
 * vec.wlock()->push_back(42);
 *
 * // Shared read — only const access is possible
 * int n = vec.rlock()->at(0);
 *
 * // Lambda-based critical section
 * vec.withWLock([](auto& v) { v.clear(); });
 * @endcode
 *
 * @tparam T     Type of the guarded datum.
 * @tparam Mutex Mutex type; defaults to `std::shared_mutex`.
 */
template <typename T, typename Mutex = std::shared_mutex>
class synchronized {
 public:
  /** @brief Default-initialise the datum and the mutex. */
  synchronized() = default;

  /** @brief Construct from an initial value (moved into the guarded storage).
   */
  explicit synchronized(T data) : data_(std::move(data)) {}

  /** @brief Copy constructor — acquires a shared lock on the source when
   *         possible, otherwise an exclusive lock. */
  synchronized(const synchronized& other) {
    if constexpr (detail::shared_mutex_c<Mutex>) {
      std::shared_lock lk(other.mutex_);
      data_ = other.data_;
    } else {
      std::unique_lock lk(other.mutex_);
      data_ = other.data_;
    }
  }

  /** @brief Copy assignment — copies the source datum under the appropriate
   *         source lock, then writes to the destination under an exclusive
   *         lock. The two mutexes are never held simultaneously. */
  synchronized& operator=(const synchronized& other) {
    if (this != &other) {
      T tmp;
      if constexpr (detail::shared_mutex_c<Mutex>) {
        std::shared_lock lk(other.mutex_);
        tmp = other.data_;
      } else {
        std::unique_lock lk(other.mutex_);
        tmp = other.data_;
      }
      std::unique_lock lk(mutex_);
      data_ = std::move(tmp);
    }
    return *this;
  }

  /** @brief Assign a new value directly under an exclusive lock. */
  synchronized& operator=(T val) {
    std::unique_lock lk(mutex_);
    data_ = std::move(val);
    return *this;
  }

  synchronized(synchronized&&) = delete;
  synchronized& operator=(synchronized&&) = delete;

  /**
   * @brief Acquire an exclusive write lock and return a mutable `locked_ptr`.
   *
   * The lock is held until the returned proxy is destroyed. Open a nested
   * scope to make the critical section clearly delimited.
   */
  auto wlock() {
    return locked_ptr<T, std::unique_lock<Mutex>>{
        &data_, std::unique_lock<Mutex>(mutex_)};
  }

  /**
   * @brief Alias for `wlock()` — provided for compatibility with
   *        exclusive-only mutex types such as `std::mutex`.
   */
  auto lock() {
    return wlock();
  }

  /**
   * @brief Execute @p fn while holding an exclusive write lock.
   *
   * The callable receives a mutable reference to the guarded datum.
   *
   * @tparam Fn  `fn(T&)` — may return a value, which is forwarded to the
   *             caller.
   */
  template <typename Fn>
  auto withWLock(Fn&& fn) {
    auto lp = wlock();
    return std::forward<Fn>(fn)(*lp);
  }

  /**
   * @brief Alias for `withWLock()`.
   */
  template <typename Fn>
  auto withLock(Fn&& fn) {
    return withWLock(std::forward<Fn>(fn));
  }

  /**
   * @brief Acquire a shared read lock and return an immutable `locked_ptr`.
   *
   * The returned proxy only grants `const` access to the guarded datum,
   * preventing modification while only a shared lock is held.
   *
   * This overload is only available when `Mutex` supports shared locking
   * (i.e. satisfies `detail::shared_mutex_c`).
   */
  auto rlock() const
    requires detail::shared_mutex_c<Mutex>
  {
    return locked_ptr<const T, std::shared_lock<Mutex>>{
        &data_, std::shared_lock<Mutex>(mutex_)};
  }

  /**
   * @brief Execute @p fn while holding a shared read lock.
   *
   * The callable receives a `const` reference to the guarded datum. Only
   * available when `Mutex` supports shared locking.
   *
   * @tparam Fn  `fn(const T&)` — may return a value, which is forwarded to
   *             the caller.
   */
  template <typename Fn>
  auto withRLock(Fn&& fn) const
    requires detail::shared_mutex_c<Mutex>
  {
    auto lp = rlock();
    return std::forward<Fn>(fn)(*lp);
  }

  /**
   * @brief Return a copy of the guarded datum taken under the least-intrusive
   *        lock available (shared when possible, exclusive otherwise).
   */
  T copy() const {
    if constexpr (detail::shared_mutex_c<Mutex>) {
      auto lp = rlock();
      return *lp;
    } else {
      std::unique_lock<Mutex> lk(mutex_);
      return data_;
    }
  }

  /**
   * @brief Write a copy of the datum into @p out under the least-intrusive
   *        lock available.
   * @param out Target to copy into.
   */
  void copy(T* out) const {
    if constexpr (detail::shared_mutex_c<Mutex>) {
      auto lp = rlock();
      *out = *lp;
    } else {
      std::unique_lock<Mutex> lk(mutex_);
      *out = data_;
    }
  }

 private:
  mutable Mutex mutex_;
  T data_;
};

/**
 * @brief Base class for arbitrary user-defined data that can be attached to a
 *        `dynamic` object without being serialized.
 *
 * Derive from `userdata` to attach application-specific context (e.g. a
 * database handle, a render state, a session token) to any `dynamic` object.
 * The attached data is accessible via `dynamic::getUserdata()` and
 * `dynamic::setUserdata()`, but it is *not* included in the serialized binary
 * output.
 *
 * @code{.cpp}
 * class MyCtx : public userdata { public: int session = 0; };
 * obj.setUserdata(std::make_shared<MyCtx>());
 * auto ctx = std::dynamic_pointer_cast<MyCtx>(obj.getUserdata());
 * @endcode
 */
class userdata {
 public:
  virtual ~userdata() = default;
};

/**
 * @brief In-memory serializer that writes directly to a byte buffer.
 *
 * `buffer_serializer` provides the same interface as `stream_serializer` but
 * avoids the virtual-dispatch overhead of `std::ostream`.  It writes bytes
 * directly into a heap-allocated buffer, which is significantly faster when the
 * bottleneck is many small `write()` calls.
 *
 * The internal buffer is grown automatically.  Use `buffer()` to obtain a
 * read-only view or `release()` to take ownership of the accumulated bytes.
 *
 * ### Example
 * @code{.cpp}
 * buffer_serializer out;
 * out.write(int32_t{42}).write(std::string{"hello"});
 * buffer bytes = out.release();
 * @endcode
 */
class buffer_serializer {
 public:
  /** @brief Construct an empty serializer; reserves @p initial_capacity bytes.
   */
  explicit buffer_serializer(size_t initial_capacity = 256) {
    buf_.reserve(initial_capacity);
  }
  buffer_serializer(const buffer_serializer&) = delete;
  buffer_serializer(buffer_serializer&&) = default;

  /** @brief Return a read-only view of the accumulated bytes. */
  const bdg::bison::buffer& buffer() const {
    return buf_;
  }

  /** @brief Move the accumulated bytes out of the serializer. */
  bdg::bison::buffer release() {
    return std::move(buf_);
  }

  /**
   * @brief Write a single scalar value in big-endian byte order.
   *
   * @tparam T  An integral or floating-point type.
   * @param  data  The value to write.
   * @return Reference to `*this` for method chaining.
   */
  template <typename T>
  buffer_serializer& write(T data) {
    data = byte_swap(data);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&data);
    buf_.insert(buf_.end(), p, p + sizeof(T));
    return *this;
  }

  /**
   * @brief Write a `std::vector` prefixed with its element count.
   *
   * On little-endian platforms the entire element array is byte-swapped into
   * a temporary buffer and written in a single insertion, reducing the number
   * of individual `insert` calls.
   *
   * @tparam T  Element type of the vector.
   * @param  data  The vector to write.
   * @return Reference to `*this` for method chaining.
   */
  template <typename T>
  buffer_serializer& write(const std::vector<T>& data) {
    size_t count = byte_swap(data.size());
    const uint8_t* cp = reinterpret_cast<const uint8_t*>(&count);
    buf_.insert(buf_.end(), cp, cp + sizeof(size_t));
    if constexpr (std::is_same_v<T, bool>) {
      // std::vector<bool> has no contiguous storage; iterate element by
      // element.
      for (bool elem : data) {
        unsigned char val = elem ? 1u : 0u;
        buf_.push_back(static_cast<uint8_t>(val));
      }
    } else if constexpr (sizeof(T) == 1) {
      // Single-byte elements need no swapping; copy in bulk.
      const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
      buf_.insert(buf_.end(), p, p + data.size());
    } else if (endian::native == endian::big) {
      // Big-endian: no byte-swap needed; copy in bulk.
      const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
      buf_.insert(buf_.end(), p, p + data.size() * sizeof(T));
    } else {
      // Little-endian: swap each element individually.
      for (const auto& elem : data) {
        T value = byte_swap(elem);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
        buf_.insert(buf_.end(), p, p + sizeof(T));
      }
    }
    return *this;
  }

  /**
   * @brief Write a `std::span` prefixed with its element count.
   *
   * @tparam T  Element type of the span.
   * @param data  The span to write.
   * @return Reference to `*this` for method chaining.
   */
  template <typename T>
  buffer_serializer& write(std::span<const T> data) {
    size_t count = byte_swap(data.size());
    const uint8_t* cp = reinterpret_cast<const uint8_t*>(&count);
    buf_.insert(buf_.end(), cp, cp + sizeof(size_t));
    if constexpr (sizeof(T) == 1) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
      buf_.insert(buf_.end(), p, p + data.size());
    } else if (endian::native == endian::big) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
      buf_.insert(buf_.end(), p, p + data.size() * sizeof(T));
    } else {
      for (const auto& elem : data) {
        T value = byte_swap(elem);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
        buf_.insert(buf_.end(), p, p + sizeof(T));
      }
    }
    return *this;
  }

  /**
   * @brief Write a `std::string` prefixed with its byte length.
   *
   * @param data  The string to write.
   * @return Reference to `*this` for method chaining.
   */
  buffer_serializer& write(const std::string& data) {
    size_t count = byte_swap(data.size());
    const uint8_t* cp = reinterpret_cast<const uint8_t*>(&count);
    buf_.insert(buf_.end(), cp, cp + sizeof(size_t));
    buf_.insert(buf_.end(), data.begin(), data.end());
    return *this;
  }

  /**
   * @brief Write a `std::string_view` prefixed with its byte length.
   *
   * @param data  The string view to write.
   * @return Reference to `*this` for method chaining.
   */
  buffer_serializer& write(std::string_view data) {
    size_t count = byte_swap(data.size());
    const uint8_t* cp = reinterpret_cast<const uint8_t*>(&count);
    buf_.insert(buf_.end(), cp, cp + sizeof(size_t));
    buf_.insert(buf_.end(), data.begin(), data.end());
    return *this;
  }

  /**
   * @brief Write raw bytes without any transformation.
   *
   * @param data   Pointer to the byte buffer.
   * @param count  Number of bytes to write.
   * @return Reference to `*this` for method chaining.
   */
  buffer_serializer& write(const char* data, std::streamsize count) {
    auto begin = reinterpret_cast<const uint8_t*>(data);
    buf_.insert(buf_.end(), begin, begin + count);
    return *this;
  }

 private:
  bdg::bison::buffer buf_;
};

/**
 * @brief In-memory deserializer that reads directly from a raw byte buffer.
 *
 * `buffer_deserializer` provides the same interface as `stream_deserializer`
 * but reads from a caller-supplied `const char*` / length pair (or
 * `buffer`) instead of an `std::istream`, avoiding virtual-dispatch
 * overhead and eliminating unnecessary heap allocations.
 *
 * ### Example
 * @code{.cpp}
 * buffer bytes = serialize_something();
 * buffer_deserializer in(bytes);
 * int32_t v = in.read<int32_t>();
 * @endcode
 */
class buffer_deserializer {
 public:
  /**
   * @brief Construct from a raw byte range.
   *
   * @param data  Pointer to the first byte.
   * @param size  Number of bytes available.
   */
  buffer_deserializer(const char* data, size_t size)
      : begin_(reinterpret_cast<const uint8_t*>(data)),
        end_(begin_ + size),
        pos_(begin_) {}

  buffer_deserializer(const uint8_t* data, size_t size)
      : begin_(data), end_(data + size), pos_(data) {}

  /** @brief Construct from a `buffer`. */
  explicit buffer_deserializer(const bdg::bison::buffer& buf)
      : buffer_deserializer(buf.data(), buf.size()) {}

  /** @brief Construct from a `std::string`. */
  explicit buffer_deserializer(const std::string& buf)
      : buffer_deserializer(buf.data(), buf.size()) {}

  buffer_deserializer(const buffer_deserializer&) = delete;
  buffer_deserializer(buffer_deserializer&&) = default;

  /**
   * @brief Read a single scalar value and return it by value.
   *
   * @tparam T  An integral or floating-point type.
   * @return The byte-swapped value read from the buffer.
   * @throws std::runtime_error on buffer underflow.
   */
  template <typename T>
  T read() {
    if (pos_ + sizeof(T) > end_) {
      throw std::runtime_error("buffer_deserializer: buffer underflow");
    }
    T data{};
    std::memcpy(&data, pos_, sizeof(T));
    pos_ += sizeof(T);
    return byte_swap(data);
  }

  /**
   * @brief Read a single scalar value into an existing variable.
   *
   * @tparam T  An integral or floating-point type.
   * @param  data  Output variable that receives the value.
   * @return Reference to `*this` for method chaining.
   */
  template <typename T>
  buffer_deserializer& read(T& data) {
    data = read<T>();
    return *this;
  }

  /**
   * @brief Read a size-prefixed vector.
   *
   * @tparam T  Element type of the vector.
   * @param  data  Output vector that receives the elements.
   * @return Reference to `*this` for method chaining.
   */
  template <typename T>
  buffer_deserializer& read(std::vector<T>& data) {
    const size_t count = read<size_t>();
    data.resize(count);
    if constexpr (std::is_same_v<T, bool>) {
      // std::vector<bool> has no contiguous storage; iterate element by
      // element.
      for (size_t idx = 0; idx < count; ++idx) {
        data[idx] = (read<unsigned char>() != 0u);
      }
    } else if constexpr (sizeof(T) == 1) {
      if (pos_ + count > end_) {
        throw std::runtime_error("buffer_deserializer: buffer underflow");
      }
      std::memcpy(data.data(), pos_, count);
      pos_ += count;
    } else if (endian::native == endian::big) {
      const size_t bytes = count * sizeof(T);
      if (pos_ + bytes > end_) {
        throw std::runtime_error("buffer_deserializer: buffer underflow");
      }
      std::memcpy(data.data(), pos_, bytes);
      pos_ += bytes;
    } else {
      for (size_t idx = 0; idx < count; ++idx) {
        data[idx] = read<T>();
      }
    }
    return *this;
  }

  /**
   * @brief Read a size-prefixed sequence into a fixed-size span.
   *
   * @tparam T  Element type of the span.
   * @param data  Output span that receives the elements.
   * @return Reference to `*this` for method chaining.
   * @throws std::runtime_error if the serialized element count does not match
   *         the span size.
   */
  template <typename T>
  buffer_deserializer& read(std::span<T> data) {
    const size_t count = read<size_t>();
    if (count != data.size()) {
      throw std::runtime_error("buffer_deserializer: Invalid span size");
    }
    for (size_t idx = 0; idx < count; ++idx) {
      data[idx] = read<T>();
    }
    return *this;
  }

  /**
   * @brief Read a size-prefixed string.
   *
   * @param data  Output string that receives the characters.
   * @return Reference to `*this` for method chaining.
   */
  buffer_deserializer& read(std::string& data) {
    const size_t count = read<size_t>();
    if (pos_ + count > end_) {
      throw std::runtime_error("buffer_deserializer: buffer underflow");
    }
    data.assign(reinterpret_cast<const char*>(pos_), count);
    pos_ += count;
    return *this;
  }

  /**
   * @brief Read a size-prefixed string into caller-provided storage and
   *        expose it as `std::string_view`.
   *
   * @param view     Output string_view over @p storage.
   * @param storage  Output string that owns the characters.
   * @return Reference to `*this` for method chaining.
   */
  buffer_deserializer& read(std::string_view& view, std::string& storage) {
    read(storage);
    view = storage;
    return *this;
  }

  /**
   * @brief Read raw bytes without any transformation.
   *
   * @param data   Buffer that receives the bytes.
   * @param count  Number of bytes to read.
   * @return Reference to `*this` for method chaining.
   */
  buffer_deserializer& read(char* data, std::streamsize count) {
    if (pos_ + count > end_) {
      throw std::runtime_error("buffer_deserializer: buffer underflow");
    }
    std::memcpy(data, pos_, count);
    pos_ += count;
    return *this;
  }

 private:
  const uint8_t* begin_;
  const uint8_t* end_;
  const uint8_t* pos_;
};

/**
 * @brief Writes primitive values, strings, and vectors to a binary stream.
 *
 * `stream_serializer` wraps an `std::ostream` and provides type-safe `write`
 * overloads. Every multi-byte scalar is byte-swapped to big-endian (network)
 * byte order before being written. Strings and vectors are prefixed with a
 * `size_t` element count so that the corresponding `stream_deserializer` can
 * reconstruct them without external length information.
 *
 * The class is non-copyable and non-movable to prevent accidental aliasing of
 * the underlying stream.
 *
 * ### Example
 * @code{.cpp}
 * std::ofstream file("data.bin", std::ios::binary);
 * stream_serializer out(file);
 * out.write(int32_t{42})
 *    .write(std::string{"hello"})
 *    .write(std::vector<float>{1.0f, 2.0f});
 * @endcode
 */
class stream_serializer {
 public:
  /** @brief Construct a stream_serializer that writes to @p out. */
  stream_serializer(std::ostream& out) : out_(out) {}
  stream_serializer(const stream_serializer& that) = delete;
  stream_serializer(stream_serializer&& that) = delete;

  /**
   * @brief Write a single scalar value in big-endian byte order.
   *
   * @tparam T  An integral or floating-point type.
   * @param  data  The value to write.
   * @return Reference to `*this` for method chaining.
   */
  template <typename T>
  stream_serializer& write(T data) {
    data = byte_swap(data);
    out_.write(reinterpret_cast<const char*>(&data), sizeof(T));
    return *this;
  }

  /**
   * @brief Write a `std::vector` prefixed with its element count.
   *
   * The count is written as a byte-swapped `size_t`, followed by each element
   * individually byte-swapped.
   *
   * @tparam T  Element type of the vector.
   * @param  data  The vector to write.
   * @return Reference to `*this` for method chaining.
   */
  template <typename T>
  stream_serializer& write(const std::vector<T>& data) {
    buffer_serializer buffered;
    buffered.write(data);
    const auto& bytes = buffered.buffer();
    return write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
  }

  /**
   * @brief Write a `std::span` prefixed with its element count.
   *
   * The count is written as a byte-swapped `size_t`, followed by each element
   * individually byte-swapped.
   *
   * @tparam T  Element type of the span.
   * @param data  The span to write.
   * @return Reference to `*this` for method chaining.
   */
  template <typename T>
  stream_serializer& write(std::span<const T> data) {
    buffer_serializer buffered;
    buffered.write(data);
    const auto& bytes = buffered.buffer();
    return write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
  }

  /**
   * @brief Write a `std::string` prefixed with its byte length.
   *
   * The length is written as a byte-swapped `size_t`, followed by the raw
   * character data (not null-terminated).
   *
   * @param data  The string to write.
   * @return Reference to `*this` for method chaining.
   */
  stream_serializer& write(const std::string& data) {
    buffer_serializer buffered;
    buffered.write(data);
    const auto& bytes = buffered.buffer();
    return write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
  }

  /**
   * @brief Write a `std::string_view` prefixed with its byte length.
   *
   * The length is written as a byte-swapped `size_t`, followed by the raw
   * character data (not null-terminated).
   *
   * @param data  The string view to write.
   * @return Reference to `*this` for method chaining.
   */
  stream_serializer& write(std::string_view data) {
    buffer_serializer buffered;
    buffered.write(data);
    const auto& bytes = buffered.buffer();
    return write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
  }

  /**
   * @brief Write raw bytes without any transformation.
   *
   * @param data   Pointer to the byte buffer.
   * @param count  Number of bytes to write.
   * @return Reference to `*this` for method chaining.
   */
  stream_serializer& write(const char* data, std::streamsize count) {
    out_.write(data, count);
    return *this;
  }

 private:
  std::ostream& out_;
};

/**
 * @brief Reads primitive values, strings, and vectors from a binary stream.
 *
 * `stream_deserializer` wraps an `std::istream` and provides type-safe `read`
 * overloads that mirror those of `stream_serializer`. Every multi-byte scalar
 * is byte-swapped from big-endian (network) byte order after being read.
 * Strings and vectors are reconstructed using the size prefix written by
 * `stream_serializer`.
 *
 * The class is non-copyable and non-movable to prevent accidental aliasing of
 * the underlying stream.
 *
 * ### Example
 * @code{.cpp}
 * std::ifstream file("data.bin", std::ios::binary);
 * stream_deserializer in(file);
 * int32_t value = in.read<int32_t>();
 * std::string text;
 * in.read(text);
 * @endcode
 */
class stream_deserializer {
 public:
  /** @brief Construct a stream_deserializer that reads from @p in. */
  stream_deserializer(std::istream& in) : in_(in) {}
  stream_deserializer(const stream_deserializer& that) = delete;
  stream_deserializer(stream_deserializer&& that) = delete;

  /**
   * @brief Read a single scalar value and return it by value.
   *
   * @tparam T  An integral or floating-point type.
   * @return The byte-swapped value read from the stream.
   */
  template <typename T>
  T read() {
    T data{};
    in_.read(reinterpret_cast<char*>(&data), sizeof(T));
    data = byte_swap(data);
    return data;
  }

  /**
   * @brief Read a single scalar value into an existing variable.
   *
   * @tparam T  An integral or floating-point type.
   * @param  data  Output variable that receives the value.
   * @return Reference to `*this` for method chaining.
   */
  template <typename T>
  stream_deserializer& read(T& data) {
    in_.read(reinterpret_cast<char*>(&data), sizeof(T));
    data = byte_swap(data);
    return *this;
  }

  /**
   * @brief Read a size-prefixed vector.
   *
   * Reads the element count (a byte-swapped `size_t`), resizes @p data, then
   * reads and byte-swaps each element individually.
   *
   * @tparam T  Element type of the vector.
   * @param  data  Output vector that receives the elements.
   * @return Reference to `*this` for method chaining.
   */
  template <typename T>
  stream_deserializer& read(std::vector<T>& data) {
    size_t count_be = 0;
    in_.read(reinterpret_cast<char*>(&count_be), sizeof(size_t));
    const size_t count = byte_swap(count_be);

    size_t payload_size = 0;
    if constexpr (std::is_same_v<T, bool>) {
      payload_size = count;
    } else {
      payload_size = count * sizeof(T);
    }

    buffer chunk(sizeof(size_t) + payload_size);
    std::memcpy(chunk.data(), &count_be, sizeof(size_t));
    if (payload_size > 0) {
      in_.read(
          reinterpret_cast<char*>(chunk.data() + sizeof(size_t)),
          static_cast<std::streamsize>(payload_size));
    }

    buffer_deserializer buffered(chunk);
    buffered.read(data);
    return *this;
  }

  /**
   * @brief Read a size-prefixed sequence into a fixed-size span.
   *
   * Reads the element count (a byte-swapped `size_t`) and verifies it matches
   * `data.size()`, then reads and byte-swaps each element into the span.
   *
   * @tparam T  Element type of the span.
   * @param data  Output span that receives the elements.
   * @return Reference to `*this` for method chaining.
   * @throws std::runtime_error if the serialized element count does not match
   *         the span size.
   */
  template <typename T>
  stream_deserializer& read(std::span<T> data) {
    size_t count_be = 0;
    in_.read(reinterpret_cast<char*>(&count_be), sizeof(size_t));
    const size_t count = byte_swap(count_be);
    if (count != data.size()) {
      throw std::runtime_error("Invalid span size");
    }

    const size_t payload_size = count * sizeof(T);
    buffer chunk(sizeof(size_t) + payload_size);
    std::memcpy(chunk.data(), &count_be, sizeof(size_t));
    if (payload_size > 0) {
      in_.read(
          reinterpret_cast<char*>(chunk.data() + sizeof(size_t)),
          static_cast<std::streamsize>(payload_size));
    }

    buffer_deserializer buffered(chunk);
    buffered.read(data);
    return *this;
  }

  /**
   * @brief Read a size-prefixed string.
   *
   * Reads the byte length (a byte-swapped `size_t`), resizes @p data, then
   * reads the raw character data.
   *
   * @param data  Output string that receives the characters.
   * @return Reference to `*this` for method chaining.
   */
  stream_deserializer& read(std::string& data) {
    size_t count_be = 0;
    in_.read(reinterpret_cast<char*>(&count_be), sizeof(size_t));
    const size_t count = byte_swap(count_be);
    data.resize(count);
    if (count > 0) {
      in_.read(data.data(), static_cast<std::streamsize>(count));
    }
    return *this;
  }

  /**
   * @brief Read a size-prefixed string into caller-provided storage and expose
   *        it as `std::string_view`.
   *
   * The view aliases @p storage and remains valid while @p storage is alive
   * and unmodified.
   *
   * @param view     Output string_view over @p storage.
   * @param storage  Output string that owns the characters.
   * @return Reference to `*this` for method chaining.
   */
  stream_deserializer& read(std::string_view& view, std::string& storage) {
    read(storage);
    view = storage;
    return *this;
  }

  /**
   * @brief Read raw bytes without any transformation.
   *
   * @param data   Buffer that receives the bytes.
   * @param count  Number of bytes to read.
   * @return Reference to `*this` for method chaining.
   */
  stream_deserializer& read(char* data, std::streamsize count) {
    in_.read(data, count);
    return *this;
  }

 private:
  std::istream& in_;
};

/**
 * @brief Base class for user-defined field attributes.
 *
 * Derive from `attribute` to create typed metadata objects that can be
 * attached to `field` instances at construction time. Attributes are stored
 * in a `shared_ptr` vector alongside the field value and can be retrieved with
 * `field::findAttribute<T>()`.
 *
 * The virtual destructor makes `attribute` polymorphic so that
 * `dynamic_cast<T*>` inside `findAttribute` works correctly on derived types.
 *
 * @code{.cpp}
 * class Required : public attribute {};
 * class MinLength : public attribute {
 *   public:
 *     explicit MinLength(size_t n) : n(n) {}
 *     size_t n;
 * };
 *
 * field f{std::string{"hello"}, attr<Required>(), attr<MinLength>(3)};
 * if (auto* ml = f.findAttribute<MinLength>()) { ... }
 * @endcode
 *
 * @see attr()
 */
class attribute {
 public:
  virtual ~attribute() = default;
};

/**
 * @brief Helper that creates a `shared_ptr<const attribute>` for a concrete
 *        attribute type.
 *
 * This is the recommended way to construct attribute instances to pass to a
 * `field` constructor. A static assertion ensures that `T` derives from
 * `attribute`.
 *
 * @tparam T     Concrete attribute type (must derive from `attribute`).
 * @tparam Args  Constructor argument types forwarded to `T`.
 * @param  args  Arguments forwarded to the constructor of `T`.
 * @return `std::shared_ptr<const attribute>` owning a new `T` instance.
 *
 * @code{.cpp}
 * field f{42, attr<Required>(), attr<Range>(0, 100)};
 * @endcode
 */
template <typename T, typename... Args>
std::shared_ptr<const attribute> attr(Args&&... args) {
  static_assert(
      std::is_base_of_v<attribute, T>, "T must derive from attribute");
  return std::make_shared<T>(std::forward<Args>(args)...);
}

/**
 * @brief A type-safe, variant-based field that can hold any supported Bison
 *        value type together with optional attribute metadata.
 *
 * `field` extends `field_base` (a `std::variant`) with:
 * - **Type-checked implicit conversion** — casting to the wrong type throws
 *   `std::runtime_error`.
 * - **Typed access with default** — `as<T>()` initialises an empty field to
 *   the provided default and returns a mutable reference.
 * - **Attribute storage** — arbitrary `attribute`-derived objects can be
 *   attached at construction time and queried later without touching the
 *   variant value.
 * - **Binary serialisation** — `serialize` / `deserialize` round-trip the
 *   field (type tag + value) through a `stream_serializer` /
 * `stream_deserializer`.
 *
 * ### Supported value types
 * See `field_base` for the full list of variant alternatives.
 *
 * ### Example
 * @code{.cpp}
 * field f{int32_t{7}, attr<Required>()};
 * int32_t v = f;            // implicit conversion
 * f = int32_t{8};           // type-checked assignment
 * int32_t& ref = f.as<int32_t>();
 * if (f.is<int32_t>()) { ... }
 * if (f.findAttribute<Required>()) { ... }
 * @endcode
 */
class field : public field_base {
 public:
  friend class dynamic;

  /** @cond INTERNAL */
  /**
   * Normalises a value before storing it in `field_base`:
   * - `const char*` / `char*`   → `std::string`
   * - `std::shared_ptr<dynamic>` → `dynamic_ptr`. This preserves the
   *   canonical variant alternative type for object references.
   * - Everything else is perfect-forwarded unchanged.
   */
  template <typename T>
  static auto to_field_value(T&& value) {
    using value_type = std::decay_t<T>;
    if constexpr (
        std::is_same_v<value_type, char*> ||
        std::is_same_v<value_type, const char*>) {
      return std::string(value);
    } else if constexpr (std::is_same_v<value_type, std::shared_ptr<dynamic>>) {
      // Normalize to the canonical variant alternative type.
      return dynamic_ptr(std::forward<T>(value));
    } else {
      return std::forward<T>(value);
    }
  }
  /** @endcond */

  /** @brief Construct an empty (monostate) field. */
  field() : field_base(std::monostate{}) {}

  /**
   * @brief Construct a field with a value and optional attributes.
   *
   * `const char*` values are silently promoted to `std::string`.
   *
   * @tparam T      Value type (must be one of the `field_base` alternatives).
   * @tparam Attrs  Attribute pointer types (`shared_ptr<const attribute>`).
   * @param  value  Initial value.
   * @param  attrs  Attribute instances created with `attr<T>(...)`.
   */
  template <typename T, typename... Attrs>
  field(T value, Attrs&&... attrs)
      : field_base(to_field_value(std::forward<T>(value))),
        attributes_{std::forward<Attrs>(attrs)...} {}

  /**
   * @brief Implicit conversion to `dynamic_ptr`.
   *
   * Kept as a non-template overload so function-style casts like
   * `dynamic_ptr(field_value)` are accepted consistently across
   * compilers.
   */
  operator dynamic_ptr() const {
    return as<dynamic_ptr>();
  }

  /**
   * @brief Implicit conversion to `std::string`.
   *
   * Kept as a non-template overload for consistent support of
   * `std::string(field_value)` syntax.
   */
  operator std::string() const {
    return as<std::string>();
  }

  /**
   * @brief Implicit conversion to `const std::string&`.
   *
   * Provides zero-copy read access to the underlying string value.
   *
   * @throws std::runtime_error if the held type is not `std::string`.
   */
  operator const std::string&() const {
    return as<std::string>();
  }

  /**
   * @brief Implicit conversion to the held type.
   *
   * @tparam T  The type to convert to; must match the active alternative.
   * @throws std::runtime_error if the held type is not `T`.
   */
  template <typename T>
  operator T() const {
    if (!std::holds_alternative<T>(static_cast<const field_base&>(*this))) {
      throw std::runtime_error("Invalid type");
    }
    return std::get<T>(static_cast<const field_base&>(*this));
  }

  /**
   * @brief Type-checked assignment.
   *
   * An empty (monostate) field accepts any value type. A non-empty field only
   * accepts assignment of the same type it already holds.
   *
   * @tparam T  The value type to assign.
   * @param  value  New value.
   * @return Reference to `*this`.
   * @throws std::runtime_error if the held type is non-empty and differs from
   *         `T`.
   */
  template <typename T>
  field& operator=(const T& value) {
    auto v = to_field_value(value);
    using value_type = decltype(v);
    if (std::holds_alternative<std::monostate>(
            static_cast<const field_base&>(*this))) {
      field_base::operator=(v);
    } else if (!std::holds_alternative<value_type>(
                   static_cast<const field_base&>(*this))) {
      throw std::runtime_error("Invalid type");
    } else {
      field_base::operator=(v);
    }
    return *this;
  }

  /**
   * @brief Check whether the field currently holds a value of type `T`.
   *
   * @tparam T  Type to test against.
   * @return `true` if the active alternative is `T`, `false` otherwise.
   */
  template <typename T>
  bool is() const {
    return std::holds_alternative<T>(static_cast<const field_base&>(*this));
  }

  /**
   * @brief Return a mutable reference to the held value of type `T`.
   *
   * If the field is empty (monostate), it is first initialised to @p def.
   *
   * @tparam T   The expected value type.
   * @param  def Default value used to initialise an empty field.
   * @return Mutable reference to the `T` value stored in the field.
   * @throws std::runtime_error if the held type is non-empty and is not `T`.
   */
  template <typename T>
  T& as(T def = T{}) {
    if (std::holds_alternative<std::monostate>(
            static_cast<const field_base&>(*this))) {
      *this = def;
    } else if (!std::holds_alternative<T>(
                   static_cast<const field_base&>(*this))) {
      throw std::runtime_error("Invalid type");
    }
    return std::get<T>(static_cast<field_base&>(*this));
  }

  /**
   * @brief Return a const reference to the held value of type `T`.
   *
   * Unlike the non-const overload, this version does **not** perform lazy
   * initialisation; it simply validates the active type and returns a
   * reference.  Call the non-const overload on a mutable field if you need
   * on-demand default initialisation.
   *
   * @tparam T   The expected value type.
   * @param  def Ignored; present for API symmetry with the non-const overload.
   * @return Const reference to the `T` value stored in the field.
   * @throws std::runtime_error if the held type is not `T`.
   */
  template <typename T>
  const T& as(T def = T{}) const {
    if (!std::holds_alternative<T>(static_cast<const field_base&>(*this))) {
      throw std::runtime_error("Invalid type");
    }
    return std::get<T>(static_cast<const field_base&>(*this));
  }

  /**
   * @brief Compile-time helper that returns the variant index for type `T`.
   *
   * Used internally by the serialization switch-case to map a C++ type to its
   * numeric type tag without magic numbers.
   *
   * @tparam T     Type whose variant index is sought.
   * @tparam index Starting index for the recursive search (default 0).
   * @return The zero-based index of `T` in `field_base`, or
   *         `std::variant_size_v<field_base>` if `T` is not found.
   */
  template <typename T, std::size_t index = 0>
  constexpr static std::size_t index_of() {
    if constexpr (index == std::variant_size_v<field_base>) {
      return index;
    } else if constexpr (
        std::is_same_v<std::variant_alternative_t<index, field_base>, T>) {
      return index;
    } else {
      return index_of<T, index + 1>();
    }
  }

  /**
   * @brief Return the 8-bit wire tag for variant alternative @p T.
   *
   * The wire format stores field type tags as one byte, so this helper
   * centralizes the narrowing cast from the variant index type.
   */
  template <typename T>
  constexpr static unsigned char tag_of() {
    constexpr std::size_t idx = index_of<T>();
    static_assert(idx < 256, "field_base alternative count exceeds tag range");
    return static_cast<unsigned char>(idx);
  }

  /**
   * @brief Look up an attached attribute by its concrete type.
   *
   * @tparam T  Attribute type to look for (must derive from `attribute`).
   * @return Pointer to the first matching attribute, or `nullptr` if none.
   */
  template <typename T>
  const T* findAttribute() const {
    static_assert(
        std::is_base_of_v<attribute, T>, "T must derive from attribute");
    for (const auto& attr_ptr : attributes_) {
      if (const T* result = dynamic_cast<const T*>(attr_ptr.get())) {
        return result;
      }
    }
    return nullptr;
  }

  /**
   * @brief Serialize this field to a binary stream.
   *
   * Writes a single-byte type tag followed by the encoded value. Attributes
   * are **not** serialized.
   *
   * @param out  The stream_serializer (wraps an `std::ostream`).
   */
  inline void serialize(stream_serializer& out) const;

  /**
   * @brief Serialize this field to an in-memory buffer.
   *
   * Equivalent to `serialize(stream_serializer&)` but writes to a
   * `buffer_serializer`, avoiding `std::ostream` virtual-dispatch overhead.
   *
   * @param out  The buffer_serializer to write to.
   */
  inline void serialize(buffer_serializer& out) const;

  /**
   * @brief Deserialize a field from a binary stream.
   *
   * Reads the type tag and reconstructs the value. No attributes are restored.
   *
   * @param in  The stream_deserializer (wraps an `std::istream`).
   * @return The deserialized `field`.
   * @throws std::runtime_error if the type tag is not recognized.
   */
  inline static field deserialize(stream_deserializer& in);

  /**
   * @brief Deserialize a field from an in-memory buffer.
   *
   * Equivalent to `deserialize(stream_deserializer&)` but reads from a
   * `buffer_deserializer`.
   *
   * @param in  The buffer_deserializer to read from.
   * @return The deserialized `field`.
   * @throws std::runtime_error if the type tag is not recognized.
   */
  inline static field deserialize(buffer_deserializer& in);

 private:
  mutable std::vector<std::shared_ptr<const attribute>> attributes_;
};

/**
 * @brief The central runtime object in the Bison library.
 *
 * A `dynamic` object is a heterogeneous property bag: it holds named `field`
 * values, named callable `method`s, an optional `userdata` attachment, and a
 * reference to its own class and parent class.
 *
 * ## Field access
 * Fields are stored in a `std::map<key_t, field>` keyed by hashed name.
 * Named keys have the high bit set (≥ 0x80000000); numeric indices (0, 1,
 * 2, …) are small positive integers that do not have the high bit set.
 * `std::map` preserves key order, which ensures that numeric (array-like)
 * indices are iterated in ascending order and that `serializeWithSchema` /
 * `deserializeWithSchema` visit fields in a deterministic, reproducible
 * sequence.  `size()` returns one past the highest numeric index.
 *
 * ## Class hierarchy & inheritance
 * Named prototypes can be registered with `addClass`. Each prototype stores a
 * `__parent` field pointing to its parent class name. When a field or method
 * is not found on an instance, the lookup automatically walks the prototype
 * chain (using the global class registry) and copies the first matching entry
 * into the instance's own map for fast subsequent access.
 *
 * ## Thread safety
 * The **global class registry** is protected internally by `getRegistry()`
 * (a `synchronized<collection>`).  Multiple threads may **read** the registry
 * concurrently via `rlock()`; write operations (`addClass`) acquire an
 * exclusive lock via `wlock()`.  `findField`, `findMethod`, and `findClass`
 * hold only a shared read lock while consulting the registry.
 * The **per-instance** field and method maps are **not** thread-safe; callers
 * must synchronise concurrent access to the same `dynamic` instance themselves.
 *
 * ## Serialization
 * Two modes are available:
 * - **Standard** (`serialize` / `deserialize`) — includes field keys in the
 *   output; fully self-describing but slightly larger.
 * - **Template** (`serializeWithSchema` / `deserializeWithSchema`) — uses
 *   the registered class definition as a schema; only field values are written,
 *   which is more compact.
 *
 * @see field, dynamic_ptr, method
 */
class dynamic {
 public:
  friend class field;
  friend class dynamic_ptr;

  /** @brief Reserved field key for the class name of an object. */
  static inline constexpr hash_t CLASS = "__class"_key;
  /** @brief Reserved field key for the parent class name of an object. */
  static inline constexpr hash_t PARENT = "__parent"_key;

  /**
   * @brief Construct a dynamic object.
   *
   * @param klass     The class name of this object (hashed key, default 0 =
   *                  anonymous).
   * @param fields    Initial named fields.
   * @param userdata  Optional userdata attachment (not serialized).
   */
  dynamic(
      key_t klass = 0U,
      std::map<key_t, field>&& fields = {},
      std::shared_ptr<userdata> userdata = nullptr)
      : fields_(std::move(fields)), userdata_(userdata) {
    fields_[CLASS] = klass;
  }

  dynamic(const dynamic& that) = default;
  dynamic(dynamic&& that) noexcept = default;
  dynamic& operator=(const dynamic& that) = delete;
  dynamic& operator=(dynamic&& that) = default;
  virtual ~dynamic() {}

  /**
   * @brief Create a deep copy of this object.
   *
   * The clone has independent copies of all fields; modifications to the clone
   * do not affect the original and vice versa. Methods and userdata are
   * shallow-copied (shared ownership via `std::shared_ptr`).
   *
   * @return A new `dynamic` with the same fields, methods, and userdata.
   */
  inline dynamic clone() const {
    return dynamic(static_cast<const dynamic>(*this));
  }

  /**
   * @brief Return the number of elements in the array-like sequence.
   *
   * This is one past the highest numeric index stored in `fields_`, or 0 if
   * no numeric keys exist.
   *
   * @return Element count for the indexed portion of the object.
   */
  inline size_t size() const {
    // Named keys have the high bit set (>= 0x80000000u); numeric array
    // indices are small positive integers that do not have the high bit set.
    // Find the last numeric key by stopping just before the named-key range.
    auto it = fields_.lower_bound(key_t{0x80000000u});
    if (it == fields_.begin())
      return 0;
    --it;
    return static_cast<size_t>(it->first.id + 1);
  }

  /**
   * @brief Remove the field at numeric index @p pos.
   *
   * @param pos  Zero-based index to remove.
   * @return `true` if a field was erased, `false` if @p pos did not exist.
   */
  inline bool erase(size_t pos) {
    return fields_.erase(static_cast<hash_t>(pos)) != 0;
  }

  /**
   * @brief Return `true` if the object has no fields at all.
   */
  inline bool empty() const {
    return fields_.empty();
  }

  /**
   * @brief Remove all fields with numeric (array-like) keys.
   *
   * Named fields (keys with the high bit set, i.e. hash values) are retained.
   */
  inline void clear() {
    // Erase only numeric (array-like) keys.  Named keys have the high bit set
    // (>= 0x80000000u) and are preserved.
    fields_.erase(fields_.begin(), fields_.lower_bound(key_t{0x80000000u}));
  }

  /**
   * @brief Access a field by numeric index (array-like access).
   *
   * Creates the field if it does not exist.
   *
   * @param pos  Zero-based numeric index.
   * @return Mutable reference to the field.
   */
  inline field& operator[](size_t pos) {
    return fields_[static_cast<hash_t>(pos)];
  }

  /**
   * @brief Access a field by numeric index (const overload).
   *
   * @param pos  Zero-based numeric index.
   * @return Const reference to the field.
   */
  inline const field& operator[](size_t pos) const {
    return fields_[static_cast<hash_t>(pos)];
  }

  /**
   * @brief Access a field by name.
   *
   * If the field is not present on this instance, the parent-class chain is
   * searched and any matching field is copied into this instance's map.
   * Creates a new empty field if still not found.
   *
   * @param name  Hashed field name (use `"name"_key`).
   * @return Mutable reference to the field.
   */
  inline field& operator[](key_t name) {
    auto field = findField(name);
    return field != nullptr ? *field : fields_[name];
  }

  /**
   * @brief Access a field by name (const overload).
   *
   * @param name  Hashed field name (use `"name"_key`).
   * @return Const reference to the field.
   */
  inline const field& operator[](key_t name) const {
    auto field = findField(name);
    return field != nullptr ? *field : fields_[name];
  }

  /** @brief Alias for `operator[](key_t)`. */
  field& at(key_t name) {
    return (*this)[name];
  }

  /** @brief Alias for `operator[](key_t) const`. */
  const field& at(key_t name) const {
    return (*this)[name];
  }

  /** @brief Alias for `operator[](size_t)`. */
  field& at(size_t pos) {
    return (*this)[pos];
  }

  /** @brief Alias for `operator[](size_t) const`. */
  const field& at(size_t pos) const {
    return (*this)[pos];
  }

  /**
   * @brief Return a mutable typed reference to a named field.
   *
   * Convenience wrapper around `operator[](name).as<T>(def)`.
   *
   * @tparam T    Expected field type.
   * @param  name Hashed field name.
   * @param  def  Default value used to initialise an empty field.
   * @return Mutable reference to the `T` value.
   */
  template <typename T>
  T& as(key_t name, T def = T{}) {
    auto& field = fields_[name];
    return field.as<T>();
  }

  /**
   * @brief Return a const typed reference to a named field.
   *
   * @tparam T    Expected field type.
   * @param  name Hashed field name.
   * @param  def  Default value used to initialise an empty field.
   * @return Const reference to the `T` value.
   */
  template <typename T>
  const T& as(key_t name, T def = T{}) const {
    auto& field = fields_[name];
    return field.as<T>();
  }

  /**
   * @brief Serialize this object to a binary stream (standard mode).
   *
   * Writes the field count followed by each (key, field) pair. The output is
   * fully self-describing: no class registry is needed to deserialize it.
   *
   * @param out  The stream_serializer to write to.
   */
  inline void serialize(stream_serializer& out) const;

  /**
   * @brief Serialize this object to an in-memory buffer (standard mode).
   *
   * Equivalent to `serialize(stream_serializer&)` but uses `buffer_serializer`
   * internally, which avoids `std::ostream` virtual-dispatch overhead.
   *
   * @param out  The buffer_serializer to write to.
   */
  inline void serialize(buffer_serializer& out) const;

  /**
   * @brief Serialize this object using its registered class as a template
   *        (compact mode).
   *
   * Only field *values* are written (in the order defined by the class
   * prototype and its parents). The class name is written first so that
   * `deserializeWithSchema` can locate the correct template.
   *
   * The class must have been registered with `addClass` before calling this
   * method.
   *
   * @param out  The stream_serializer to write to.
   */
  inline void serializeWithSchema(stream_serializer& out) const;

  /**
   * @brief Serialize this object using its registered class as a template
   *        (compact mode, in-memory buffer variant).
   *
   * @param out  The buffer_serializer to write to.
   */
  inline void serializeWithSchema(buffer_serializer& out) const;

  /**
   * @brief Deserialize an object from a binary stream (standard mode).
   *
   * @param in  The stream_deserializer to read from.
   * @return The reconstructed `dynamic` object.
   */
  inline static dynamic deserialize(stream_deserializer& in);

  /**
   * @brief Deserialize an object from an in-memory buffer (standard mode).
   *
   * Equivalent to `deserialize(stream_deserializer&)` but reads from a
   * `buffer_deserializer`, which avoids `std::istream` virtual-dispatch
   * overhead.
   *
   * @param in  The buffer_deserializer to read from.
   * @return The reconstructed `dynamic` object.
   */
  inline static dynamic deserialize(buffer_deserializer& in);

  /**
   * @brief Deserialize an object using a registered class template (compact
   *        mode).
   *
   * Reads the class name, locates the registered prototype, and reconstructs
   * field values in the order defined by the prototype chain.
   *
   * @param in  The stream_deserializer to read from.
   * @return The reconstructed `dynamic` object.
   */
  inline static dynamic deserializeWithSchema(stream_deserializer& in);

  /**
   * @brief Deserialize an object using a registered class template (compact
   *        mode, in-memory buffer variant).
   *
   * @param in  The buffer_deserializer to read from.
   * @return The reconstructed `dynamic` object.
   */
  inline static dynamic deserializeWithSchema(buffer_deserializer& in);

  /**
   * @brief Add a named field to this object.
   *
   * The operation fails (returns `false`) if a field with the same name
   * already exists. Use `operator[]` to modify existing fields.
   *
   * @param name   Hashed field name.
   * @param value  Initial field value.
   * @return `true` if the field was added, `false` if the name was already
   *         taken.
   */
  inline bool addField(key_t name, field value) {
    return fields_.emplace(std::make_pair(name, std::move(value))).second;
  }

  /**
   * @brief Register a callable method on this object.
   *
   * The operation fails (returns `false`) if a method with the same name
   * already exists.
   *
   * @param name  Hashed method name.
   * @param fn    The callable (`std::function<dynamic(dynamic&, const
   * dynamic&)>`).
   * @return `true` if the method was registered, `false` if the name was
   *         already taken.
   */
  inline bool addMethod(key_t name, method fn) {
    return methods_.emplace(std::make_pair(name, fn)).second;
  }

  /**
   * @brief Attach arbitrary userdata to this object.
   *
   * @param userdata  Shared pointer to a `userdata`-derived object.
   */
  inline void setUserdata(std::shared_ptr<userdata> userdata) {
    userdata_ = std::move(userdata);
  }

  /**
   * @brief Retrieve the attached userdata, if any.
   *
   * @return Shared pointer to the `userdata` (may be null).
   */
  inline std::shared_ptr<userdata> getUserdata() const {
    return userdata_;
  }

  /**
   * @brief Invoke a named method on this object.
   *
   * Searches the instance's own method map first, then walks the parent-class
   * chain. The first matching method is called with `*this` as `self` and
   * @p params as the argument object.
   *
   * @param name    Hashed method name.
   * @param params  Argument object passed to the method as `const dynamic&`.
   * @return The `dynamic` value returned by the method.
   * @throws std::runtime_error if no method with @p name is found.
   */
  inline dynamic call(key_t name, const dynamic& params) {
    auto fn = findMethod(name);
    if (fn == nullptr) {
      throw std::runtime_error("Method not found");
    }
    return (*fn)(*this, params);
  }

  /**
   * @brief Create an anonymous instance of a named class.
   *
   * This is a lightweight factory that simply constructs a `dynamic` with the
   * given class key set. The class does **not** need to be registered for this
   * to work; field and method inheritance only resolves when those members are
   * actually accessed.
   *
   * @param klass  Hashed class name.
   * @return A new `dynamic` instance with `CLASS` set to @p klass.
   */
  static dynamic instantiate(const key_t klass) {
    return dynamic{klass};
  }

  /**
   * @brief Register a class prototype in the global class registry.
   *
   * @p klass becomes a child of @p parent. The `__parent` field of @p klass
   * is set to @p parent automatically.
   *
   * The registry checks for circular inheritance: if adding @p klass would
   * create a cycle, the registration is rejected and `false` is returned.
   *
   * @param parent  Hashed name of the parent class (0 for root).
   * @param klass   Shared pointer to the prototype `dynamic` object; its
   *                `CLASS` field must already be set to the desired name.
   * @return `true` if registration succeeded, `false` if a cycle was detected
   *         or a class with the same name already exists.
   *
   * @code{.cpp}
   * auto base = dynamic_ptr{"Animal"_key, {{"legs"_key, int32_t{4}}}};
   * dynamic::addClass(0U, base);
   * auto dog = dynamic_ptr{"Dog"_key, {{"breed"_key, std::string{"Lab"}}}};
   * dynamic::addClass("Animal"_key, dog);
   * @endcode
   */
  static bool addClass(const key_t parent, dynamic_ptr klass) {
    auto name = klass->as<key_t>(CLASS);
    (*klass)[PARENT] = parent;

    auto lp = getRegistry().wlock();
    auto ancestor = parent;
    auto it = lp->find(parent);
    while (it != lp->end() && ancestor != name) {
      ancestor = it->second->as<key_t>(PARENT);
      it = lp->find(ancestor);
    }

    if (ancestor == name) {
      return false;
    }

    return lp->try_emplace(name, std::move(klass)).second;
  }

  /**
   * @brief Find a field on this instance, searching the parent-class chain if
   *        necessary.
   *
   * When a field is found in a parent class it is **copied into this
   * instance's map** so that subsequent lookups are O(log n) without further
   * registry traversal.
   *
   * @param name  Hashed field name.
   * @return Pointer to the field, or `nullptr` if not found anywhere in the
   *         hierarchy.
   */
  field* findField(key_t name) const {
    auto it = fields_.find(name);
    if (it == fields_.end()) {
      auto lp = getRegistry().rlock();
      // Begin the search at the instance's own registered class prototype, then
      // walk up the PARENT chain stored on each prototype.
      auto itClass = lp->find(as<key_t>(CLASS));
      while (itClass != lp->end() && it == fields_.end()) {
        auto& klass = itClass->second;
        auto itField = klass->fields_.find(name);
        if (itField != klass->fields_.end()) {
          it = fields_.insert(std::make_pair(name, itField->second)).first;
        } else {
          itClass = lp->find(klass->as<key_t>(PARENT));
        }
      }
    }

    return it != fields_.end() ? &it->second : nullptr;
  }

  /**
   * @brief Find a method on this instance, searching the parent-class chain if
   *        necessary.
   *
   * When a method is found in a parent class it is cached in this instance's
   * method map.
   *
   * @param name  Hashed method name.
   * @return Pointer to the `method` functor, or `nullptr` if not found.
   */
  method* findMethod(key_t name) const {
    auto it = methods_.find(name);
    if (it == methods_.end()) {
      auto lp = getRegistry().rlock();
      // Begin the search at the instance's own registered class prototype, then
      // walk up the PARENT chain stored on each prototype.
      auto itClass = lp->find(as<key_t>(CLASS));
      while (itClass != lp->end() && it == methods_.end()) {
        auto& klass = itClass->second;
        auto itMethod = klass->methods_.find(name);
        if (itMethod != klass->methods_.end()) {
          it = methods_.insert(std::make_pair(name, itMethod->second)).first;
        } else {
          itClass = lp->find(klass->as<key_t>(PARENT));
        }
      }
    }

    return it != methods_.end() ? &it->second : nullptr;
  }

  /**
   * @brief Search this object's class chain for a registered class named
   *        @p name.
   *
   * Starts from this object's own class and walks up via `__parent` links in
   * the global registry.
   *
   * @param name  Hashed class name to search for.
   * @return Non-owning pointer to the matching prototype, or `nullptr`.
   */
  dynamic* findClass(key_t name) const {
    auto lp = getRegistry().rlock();
    auto klass = as<key_t>(CLASS);
    auto it = lp->find(klass);
    while (it != lp->end() && klass != name) {
      klass = it->second->as<key_t>(PARENT);
      it = lp->find(klass);
    }

    return it != lp->end() ? it->second.get() : nullptr;
  }

  /**
   * @brief Iterate over all fields of this object.
   *
   * Calls @p fn once for each (key, field) pair stored directly on this
   * instance.  Named fields (hashed keys ≥ 0x80000000) appear in ascending
   * hash-value order; array-like numeric fields (keys < 0x80000000) appear
   * first in ascending index order.
   *
   * Fields that are only inherited from a registered prototype and have
   * not yet been accessed (and thus not yet copied into this instance's own
   * map) are **not** visited.  Call `operator[](key_t)` on those fields first
   * if you need them to appear.
   *
   * @tparam F  Callable with signature `void(key_t, const field&)`.
   * @param  fn  Callable invoked for every stored (key, value) pair.
   */
  template <typename F>
  void forEach(F&& fn) const {
    for (const auto& kv : fields_) {
      fn(kv.first, kv.second);
    }
  }

  /**
   * @brief Return the global class registry, protected by a `synchronized`
   *        wrapper.
   *
   * Use `rlock()` for read-only access and `wlock()` for mutations.
   *
   * @return Reference to the static `synchronized<collection>`.
   */
  static inline synchronized<collection>& getRegistry() {
    static synchronized<collection> registry;
    return registry;
  }

 private:
  mutable std::map<key_t, field> fields_;
  mutable std::unordered_map<key_t, method, key_t, key_t> methods_;
  mutable std::shared_ptr<userdata> userdata_;
};

/**
 * @brief An owning smart pointer for `dynamic` objects with convenient
 *        constructors.
 *
 * `dynamic_ptr` inherits from `std::shared_ptr<dynamic>` so it is fully
 * compatible everywhere a `shared_ptr<dynamic>` is expected. It adds two extra
 * constructors:
 * - `dynamic_ptr(dynamic&&)` — takes ownership of an rvalue `dynamic`.
 * - `dynamic_ptr(key_t, std::map<key_t, field>&&)` — directly constructs the
 *   managed `dynamic` with a class key and initial fields, matching the
 *   `dynamic` constructor signature.
 *
 * ### Example
 * @code{.cpp}
 * dynamic_ptr obj{"Point"_key, {{"x"_key, 0.0f}, {"y"_key, 0.0f}}};
 * obj->serialize(stream_serializer(ss));
 * @endcode
 */
inline dynamic_ptr::dynamic_ptr(const std::shared_ptr<dynamic>& that)
    : std::shared_ptr<dynamic>(that) {}

inline dynamic_ptr::dynamic_ptr(std::shared_ptr<dynamic>&& that)
    : std::shared_ptr<dynamic>(std::move(that)) {}

inline dynamic_ptr& dynamic_ptr::operator=(
    const std::shared_ptr<dynamic>& that) {
  std::shared_ptr<dynamic>::operator=(that);
  return *this;
}

inline dynamic_ptr& dynamic_ptr::operator=(std::shared_ptr<dynamic>&& that) {
  std::shared_ptr<dynamic>::operator=(std::move(that));
  return *this;
}

inline dynamic_ptr::dynamic_ptr(dynamic&& that)
    : std::shared_ptr<dynamic>(new dynamic{std::move(that)}) {}

inline dynamic_ptr::dynamic_ptr(key_t klass, std::map<key_t, field>&& fields)
    : std::shared_ptr<dynamic>(new dynamic{klass, std::move(fields)}) {}

inline void field::serialize(stream_serializer& out) const {
  buffer_serializer buffered;
  serialize(buffered);
  const auto& bytes = buffered.buffer();
  out.write(
      reinterpret_cast<const char*>(bytes.data()),
      static_cast<std::streamsize>(bytes.size()));
}

inline field field::deserialize(stream_deserializer& in) {
  // Type tags are derived from the variant index order in field_base.
  const auto type = in.read<unsigned char>();
  switch (type) {
    case field::tag_of<std::monostate>():
      return field{std::monostate{}};
    case field::tag_of<hash_t>():
      return field{in.read<hash_t>()};
    case field::tag_of<key_t>():
      return field{in.read<key_t>()};
    case field::tag_of<bool>():
      return field{in.read<bool>()};
    case field::tag_of<int32_t>():
      return field{in.read<int32_t>()};
    case field::tag_of<float>():
      return field{in.read<float>()};
    case field::tag_of<dynamic_ptr>(): {
      if (!in.read<bool>())
        return field{std::shared_ptr<dynamic>{}};
      return field{dynamic_ptr{dynamic::deserialize(in)}};
    }
    case field::tag_of<std::string>(): {
      std::string s;
      in.read(s);
      return field{std::move(s)};
    }
    case field::tag_of<std::vector<bool>>(): {
      std::vector<bool> v;
      in.read(v);
      return field{std::move(v)};
    }
    case field::tag_of<std::vector<int32_t>>(): {
      std::vector<int32_t> v;
      in.read(v);
      return field{std::move(v)};
    }
    case field::tag_of<std::vector<float>>(): {
      std::vector<float> v;
      in.read(v);
      return field{std::move(v)};
    }
    case field::tag_of<std::vector<uint8_t>>(): {
      std::vector<uint8_t> v;
      in.read(v);
      return field{std::move(v)};
    }
    default:
      throw std::runtime_error("Not implemented");
  }
}

inline void dynamic::serialize(stream_serializer& out) const {
  buffer_serializer buffered;
  serialize(buffered);
  const auto& bytes = buffered.buffer();
  out.write(
      reinterpret_cast<const char*>(bytes.data()),
      static_cast<std::streamsize>(bytes.size()));
}

inline void dynamic::serializeWithSchema(stream_serializer& out) const {
  buffer_serializer buffered;
  serializeWithSchema(buffered);
  const auto& bytes = buffered.buffer();
  out.write(
      reinterpret_cast<const char*>(bytes.data()),
      static_cast<std::streamsize>(bytes.size()));
}

inline dynamic dynamic::deserialize(stream_deserializer& in) {
  dynamic dyn{};
  auto count = in.read<size_t>();
  for (size_t i = 0; i < count; ++i) {
    auto key = in.read<key_t>();
    dyn.fields_[key] = field::deserialize(in);
  }

  return dyn;
}

inline dynamic dynamic::deserializeWithSchema(stream_deserializer& in) {
  dynamic dyn{};
  auto klass = in.read<key_t>();
  auto lp = getRegistry().rlock();
  auto it = lp->find(klass);

  while (it != lp->end()) {
    for (const auto& kv : it->second->fields_) {
      dyn.fields_[kv.first] = field::deserialize(in);
    }
    klass = it->second->as<key_t>(PARENT);
    it = lp->find(klass);
  }

  return dyn;
}

inline void field::serialize(buffer_serializer& out) const {
  const field_base& fb = static_cast<const field_base&>(*this);
  out.write(static_cast<unsigned char>(fb.index()));
  std::visit(
      [&out](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          // nothing to write for an empty field
        } else if constexpr (std::is_same_v<T, dynamic_ptr>) {
          out.write(v != nullptr);
          if (v != nullptr) {
            v->serialize(out);
          }
        } else {
          out.write(v);
        }
      },
      fb);
}

inline field field::deserialize(buffer_deserializer& in) {
  const auto type = in.read<unsigned char>();
  switch (type) {
    case field::tag_of<std::monostate>():
      return field{std::monostate{}};
    case field::tag_of<hash_t>():
      return field{in.read<hash_t>()};
    case field::tag_of<key_t>():
      return field{in.read<key_t>()};
    case field::tag_of<bool>():
      return field{in.read<bool>()};
    case field::tag_of<int32_t>():
      return field{in.read<int32_t>()};
    case field::tag_of<float>():
      return field{in.read<float>()};
    case field::tag_of<dynamic_ptr>(): {
      if (!in.read<bool>())
        return field{std::shared_ptr<dynamic>{}};
      return field{dynamic_ptr{dynamic::deserialize(in)}};
    }
    case field::tag_of<std::string>(): {
      std::string s;
      in.read(s);
      return field{std::move(s)};
    }
    case field::tag_of<std::vector<bool>>(): {
      std::vector<bool> v;
      in.read(v);
      return field{std::move(v)};
    }
    case field::tag_of<std::vector<int32_t>>(): {
      std::vector<int32_t> v;
      in.read(v);
      return field{std::move(v)};
    }
    case field::tag_of<std::vector<float>>(): {
      std::vector<float> v;
      in.read(v);
      return field{std::move(v)};
    }
    case field::tag_of<std::vector<uint8_t>>(): {
      std::vector<uint8_t> v;
      in.read(v);
      return field{std::move(v)};
    }
    default:
      throw std::runtime_error("Not implemented");
  }
}

inline void dynamic::serialize(buffer_serializer& out) const {
  out.write(fields_.size());
  for (const auto& kv : fields_) {
    out.write(kv.first);
    kv.second.serialize(out);
  }
}

inline void dynamic::serializeWithSchema(buffer_serializer& out) const {
  auto lp = getRegistry().rlock();
  auto klass = as<key_t>(CLASS);
  auto it = lp->find(klass);

  out.write(klass);
  while (it != lp->end()) {
    for (const auto& kv : it->second->fields_) {
      auto instance_it = fields_.find(kv.first);
      if (instance_it != fields_.end()) {
        instance_it->second.serialize(out);
      } else {
        kv.second.serialize(out);
      }
    }
    klass = it->second->as<key_t>(PARENT);
    it = lp->find(klass);
  }
}

inline dynamic dynamic::deserialize(buffer_deserializer& in) {
  dynamic dyn{};
  const auto count = in.read<size_t>();
  for (size_t i = 0; i < count; ++i) {
    auto key = in.read<key_t>();
    dyn.fields_[key] = field::deserialize(in);
  }
  return dyn;
}

inline dynamic dynamic::deserializeWithSchema(buffer_deserializer& in) {
  dynamic dyn{};
  auto klass = in.read<key_t>();
  auto lp = getRegistry().rlock();
  auto it = lp->find(klass);

  while (it != lp->end()) {
    for (const auto& kv : it->second->fields_) {
      dyn.fields_[kv.first] = field::deserialize(in);
    }
    klass = it->second->as<key_t>(PARENT);
    it = lp->find(klass);
  }

  return dyn;
}

/**
 * @namespace bdg::bison::extensions
 * @brief Optional extensions that add interoperability with other formats.
 *
 * Currently supported import formats:
 * | Function | Format | Dependency |
 * |---|---|---|
 * | `from_json`    | JSON              | nlohmann/json (bundled) |
 * | `from_yaml`    | YAML 1.1/1.2      | libyaml (bundled) |
 */
namespace extensions {
/**
 * @brief Parse a JSON string and return it as a `dynamic` object.
 *
 * The conversion follows these rules:
 *
 * | JSON type | `field` type |
 * |---|---|
 * | `null` | `std::shared_ptr<dynamic>{}` (null ptr) |
 * | `boolean` | `bool` |
 * | `integer` | `int32_t` |
 * | `float` | `float` |
 * | `string` | `std::string` |
 * | `array` | `dynamic` with zero-based numeric indices |
 * | `object` | `dynamic` with hashed-string keys |
 *
 * @param json  A valid JSON string.
 * @return Shared pointer to the root `dynamic` object.
 * @throws `nlohmann::json::parse_error` if @p json is not valid JSON.
 *
 * @code{.cpp}
 * auto obj = extensions::from_json(R"({"x": 1, "y": 2.5})");
 * int32_t x = (*obj)["x"].as<int32_t>();
 * @endcode
 */
dynamic_ptr from_json(std::string json);

/**
 * @brief Parse a YAML string and return it as a `dynamic` object.
 *
 * Uses libyaml for parsing.  The type-coercion rules for plain (unquoted)
 * scalars follow YAML 1.1 core schema conventions:
 *
 * | YAML value | `field` type |
 * |---|---|
 * | `null` / `~` / empty | `std::shared_ptr<dynamic>{}` (null ptr) |
 * | `true` / `yes` / `on` | `bool` (`true`) |
 * | `false` / `no` / `off` | `bool` (`false`) |
 * | integer literal | `int32_t` |
 * | floating-point literal | `float` |
 * | quoted or other string | `std::string` |
 * | sequence | `dynamic` with zero-based numeric indices |
 * | mapping | `dynamic` with hashed-string keys |
 *
 * @param yaml  A valid YAML string (UTF-8).
 * @return Shared pointer to the root `dynamic` object.
 * @throws `std::runtime_error` if @p yaml is not valid YAML.
 *
 * @code{.cpp}
 * auto obj = extensions::from_yaml("x: 1\ny: 2.5\n");
 * int32_t x = (*obj)["x"].as<int32_t>();
 * @endcode
 */
dynamic_ptr from_yaml(std::string yaml);

} // namespace extensions

} // namespace bdg::bison
