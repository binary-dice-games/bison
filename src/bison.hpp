// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>

/**
 * @file bison.hpp
 * @brief Bison — self-describing binary serialization and dynamic object library.
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
 * obj.serialize(serializer(ss));
 * auto copy = dynamic::deserialize(deserializer(ss));
 * @endcode
 */

/**
 * @namespace bdg::bison
 * @brief Root namespace for the Bison library.
 *
 * Contains all public types, free functions, and the `extensions` sub-namespace.
 */
namespace bdg::bison {

class serializer;
class deserializer;
class attribute;
class field;
class dynamic;
class dynamic_ptr;

/**
 * @namespace bdg::bison::endian
 * @brief Compile-time and runtime endianness constants.
 *
 * Use these constants together with `byte_swap` to write portable
 * serialization code. `endian::native` is evaluated once at program start and
 * reflects the byte order of the current platform.
 */
namespace endian {
/** @brief Big-endian / network byte order (value = 1). This is the reference byte order used on the wire. */
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
 * @tparam T  An integral or floating-point type.
 * @param  value  The value whose bytes may be reversed.
 * @return The byte-swapped value (or the original value on big-endian hosts).
 */
template <typename T>
constexpr T byte_swap(T value) {
  if (endian::native == endian::big) {
    return value;
  } else {
    T result = 0U;
    const size_t size = sizeof(T);
    for (size_t i = 0; i < size; ++i) {
      ((unsigned char*)&result)[size - i - 1] = ((unsigned char*)&value)[i];
    }
    return result;
  }
}

/** @brief Unsigned 32-bit integer used as a hashed field key. */
using hash_t = uint32_t;

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
 * | 6 | `std::shared_ptr<dynamic>` |
 * | 7 | `std::string` |
 * | 8 | `std::vector<bool>` |
 * | 9 | `std::vector<int32_t>` |
 * | 10 | `std::vector<float>` |
 */
using field_base = std::variant<
    std::monostate,
    hash_t,
    key_t,
    bool,
    int32_t,
    float,
    std::shared_ptr<dynamic>,
    std::string,
    std::vector<bool>,
    std::vector<int32_t>,
    std::vector<float>>;

/**
 * @brief Map type used for the global class registry.
 *
 * Maps a class name (as a `key_t`) to its shared `dynamic` prototype.
 */
using collection =
    std::unordered_map<key_t, std::shared_ptr<dynamic>, key_t, key_t>;

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
 * @brief Writes primitive values, strings, and vectors to a binary stream.
 *
 * `serializer` wraps an `std::ostream` and provides type-safe `write`
 * overloads. Every multi-byte scalar is byte-swapped to big-endian (network)
 * byte order before being written. Strings and vectors are prefixed with a
 * `size_t` element count so that the corresponding `deserializer` can
 * reconstruct them without external length information.
 *
 * The class is non-copyable and non-movable to prevent accidental aliasing of
 * the underlying stream.
 *
 * ### Example
 * @code{.cpp}
 * std::ofstream file("data.bin", std::ios::binary);
 * serializer out(file);
 * out.write(int32_t{42})
 *    .write(std::string{"hello"})
 *    .write(std::vector<float>{1.0f, 2.0f});
 * @endcode
 */
class serializer {
 public:
  /** @brief Construct a serializer that writes to @p out. */
  serializer(std::ostream& out) : out_(out) {}
  serializer(const serializer& that) = delete;
  serializer(serializer&& that) = delete;

  /**
   * @brief Write a single scalar value in big-endian byte order.
   *
   * @tparam T  An integral or floating-point type.
   * @param  data  The value to write.
   * @return Reference to `*this` for method chaining.
   */
  template <typename T>
  serializer& write(T data) {
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
  serializer& write(const std::vector<T>& data) {
    size_t count = byte_swap(data.size());
    out_.write(reinterpret_cast<const char*>(&count), sizeof(size_t));
    for (auto it : data) {
      T value = byte_swap(it);
      out_.write(reinterpret_cast<const char*>(&value), sizeof(T));
    }
    return *this;
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
  serializer& write(const std::string& data) {
    size_t count = byte_swap(data.size());
    out_.write(reinterpret_cast<const char*>(&count), sizeof(size_t));
    out_.write(data.data(), data.size());
    return *this;
  }

  /**
   * @brief Write raw bytes without any transformation.
   *
   * @param data   Pointer to the byte buffer.
   * @param count  Number of bytes to write.
   * @return Reference to `*this` for method chaining.
   */
  serializer& write(const char* data, std::streamsize count) {
    out_.write(data, count);
    return *this;
  }

 private:
  std::ostream& out_;
};

/**
 * @brief Reads primitive values, strings, and vectors from a binary stream.
 *
 * `deserializer` wraps an `std::istream` and provides type-safe `read`
 * overloads that mirror those of `serializer`. Every multi-byte scalar is
 * byte-swapped from big-endian (network) byte order after being read. Strings
 * and vectors are reconstructed using the size prefix written by `serializer`.
 *
 * The class is non-copyable and non-movable to prevent accidental aliasing of
 * the underlying stream.
 *
 * ### Example
 * @code{.cpp}
 * std::ifstream file("data.bin", std::ios::binary);
 * deserializer in(file);
 * int32_t value = in.read<int32_t>();
 * std::string text;
 * in.read(text);
 * @endcode
 */
class deserializer {
 public:
  /** @brief Construct a deserializer that reads from @p in. */
  deserializer(std::istream& in) : in_(in) {}
  deserializer(const deserializer& that) = delete;
  deserializer(deserializer&& that) = delete;

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
  deserializer& read(T& data) {
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
  deserializer& read(std::vector<T>& data) {
    size_t count = 0;
    in_.read(reinterpret_cast<char*>(&count), sizeof(size_t));
    count = byte_swap(count);
    data.resize(count);
    for (size_t idx = 0; idx < count; ++idx) {
      T value{};
      in_.read(reinterpret_cast<char*>(&value), sizeof(T));
      data[idx] = byte_swap(value);
    }
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
  deserializer& read(std::string& data) {
    size_t count = 0;
    in_.read(reinterpret_cast<char*>(&count), sizeof(size_t));
    count = byte_swap(count);
    data.resize(count);
    in_.read(data.data(), data.size());
    return *this;
  }

  /**
   * @brief Read raw bytes without any transformation.
   *
   * @param data   Buffer that receives the bytes.
   * @param count  Number of bytes to read.
   * @return Reference to `*this` for method chaining.
   */
  deserializer& read(char* data, std::streamsize count) {
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
 *   field (type tag + value) through a `serializer` / `deserializer`.
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
   * - Any type that is implicitly convertible to `std::shared_ptr<dynamic>`
   *   but is not already `std::shared_ptr<dynamic>` (e.g. `dynamic_ptr`) →
   *   `std::shared_ptr<dynamic>`.  This allows `dynamic_ptr` to be assigned
   *   to a field without an explicit cast.
   * - Everything else is perfect-forwarded unchanged.
   */
  template <typename T>
  static auto to_field_value(T&& value) {
    using value_type = std::decay_t<T>;
    if constexpr (
        std::is_same_v<value_type, char*> ||
        std::is_same_v<value_type, const char*>) {
      return std::string(value);
    } else if constexpr (
        !std::is_same_v<value_type, std::shared_ptr<dynamic>> &&
        std::is_convertible_v<value_type, std::shared_ptr<dynamic>>) {
      // Upcast dynamic_ptr (and any other shared_ptr<dynamic> subclass) to
      // the canonical variant alternative type.
      return std::shared_ptr<dynamic>(std::forward<T>(value));
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
    if (std::holds_alternative<std::monostate>(static_cast<const field_base&>(*this))) {
      field_base::operator=(v);
    } else if (!std::holds_alternative<value_type>(static_cast<const field_base&>(*this))) {
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
    if (std::holds_alternative<std::monostate>(static_cast<const field_base&>(*this))) {
      *this = def;
    } else if (!std::holds_alternative<T>(static_cast<const field_base&>(*this))) {
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
    } else if constexpr (std::is_same_v<
                             std::variant_alternative_t<index, field_base>,
                             T>) {
      return index;
    } else {
      return index_of<T, index + 1>();
    }
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
   * @param out  The serializer (wraps an `std::ostream`).
   */
  inline void serialize(serializer& out) const;

  /**
   * @brief Deserialize a field from a binary stream.
   *
   * Reads the type tag and reconstructs the value. No attributes are restored.
   *
   * @param in  The deserializer (wraps an `std::istream`).
   * @return The deserialized `field`.
   * @throws std::runtime_error if the type tag is not recognized.
   */
  inline static field deserialize(deserializer& in);

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
 * Numeric indices (0, 1, 2, …) are used to model ordered, array-like sequences
 * inside the same map; `size()` returns one past the highest numeric index.
 *
 * ## Class hierarchy & inheritance
 * Named prototypes can be registered with `addClass`. Each prototype stores a
 * `__parent` field pointing to its parent class name. When a field or method
 * is not found on an instance, the lookup automatically walks the prototype
 * chain (using the global class registry) and copies the first matching entry
 * into the instance's own map for fast subsequent access.
 *
 * ## Thread safety
 * The **global class registry** (`getClasses()`) is protected by `getMutex()`
 * (a `std::shared_mutex`).  Multiple threads may **read** the registry
 * concurrently using a `std::shared_lock`; write operations (`addClass`)
 * acquire an exclusive `std::unique_lock`.  `findField`, `findMethod`, and
 * `findClass` hold only a shared (read) lock while consulting the registry.
 * The **per-instance** field and method maps are **not** thread-safe; callers
 * must synchronise concurrent access to the same `dynamic` instance themselves.
 *
 * ## Serialization
 * Two modes are available:
 * - **Standard** (`serialize` / `deserialize`) — includes field keys in the
 *   output; fully self-describing but slightly larger.
 * - **Template** (`serializeWithTemplate` / `deserializeWithTemplate`) — uses
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
    if (it == fields_.begin()) return 0;
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
   * @param out  The serializer to write to.
   */
  inline void serialize(serializer& out) const;

  /**
   * @brief Serialize this object using its registered class as a template
   *        (compact mode).
   *
   * Only field *values* are written (in the order defined by the class
   * prototype and its parents). The class name is written first so that
   * `deserializeWithTemplate` can locate the correct template.
   *
   * The class must have been registered with `addClass` before calling this
   * method.
   *
   * @param out  The serializer to write to.
   */
  inline void serializeWithTemplate(serializer& out) const;

  /**
   * @brief Deserialize an object from a binary stream (standard mode).
   *
   * @param in  The deserializer to read from.
   * @return Shared pointer to the reconstructed `dynamic` object.
   */
  inline static std::shared_ptr<dynamic> deserialize(deserializer& in);

  /**
   * @brief Deserialize an object using a registered class template (compact
   *        mode).
   *
   * Reads the class name, locates the registered prototype, and reconstructs
   * field values in the order defined by the prototype chain.
   *
   * @param in  The deserializer to read from.
   * @return Shared pointer to the reconstructed `dynamic` object.
   */
  inline static std::shared_ptr<dynamic> deserializeWithTemplate(
      deserializer& in);

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
   * @param fn    The callable (`std::function<dynamic(dynamic&, const dynamic&)>`).
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
  static bool addClass(const key_t parent, std::shared_ptr<dynamic> klass) {
    std::unique_lock<std::shared_mutex> lk(getMutex());
    auto name = klass->as<key_t>(CLASS);
    (*klass)[PARENT] = parent;

    auto ancestor = parent;
    auto& classes = getClasses();
    auto it = classes.find(parent);
    while (it != classes.end() && ancestor != name) {
      ancestor = it->second->as<key_t>(PARENT);
      it = classes.find(ancestor);
    }

    if (ancestor == name) {
      return false;
    }

    return classes.try_emplace(name, std::move(klass)).second;
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
      std::shared_lock<std::shared_mutex> lk(getMutex());
      auto& classes = getClasses();
      // Begin the search at the instance's own registered class prototype, then
      // walk up the PARENT chain stored on each prototype.
      auto itClass = classes.find(as<key_t>(CLASS));
      while (itClass != classes.end() && it == fields_.end()) {
        auto& klass = itClass->second;
        auto itField = klass->fields_.find(name);
        if (itField != klass->fields_.end()) {
          it = fields_.insert(std::make_pair(name, itField->second)).first;
        } else {
          itClass = classes.find(klass->as<key_t>(PARENT));
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
      std::shared_lock<std::shared_mutex> lk(getMutex());
      auto& classes = getClasses();
      // Begin the search at the instance's own registered class prototype, then
      // walk up the PARENT chain stored on each prototype.
      auto itClass = classes.find(as<key_t>(CLASS));
      while (itClass != classes.end() && it == methods_.end()) {
        auto& klass = itClass->second;
        auto itMethod = klass->methods_.find(name);
        if (itMethod != klass->methods_.end()) {
          it = methods_.insert(std::make_pair(name, itMethod->second)).first;
        } else {
          itClass = classes.find(klass->as<key_t>(PARENT));
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
    std::shared_lock<std::shared_mutex> lk(getMutex());
    auto& classes = getClasses();
    auto klass = as<key_t>(CLASS);
    auto it = classes.find(klass);
    while (it != classes.end() && klass != name) {
      klass = it->second->as<key_t>(PARENT);
      it = classes.find(klass);
    }

    return it != classes.end() ? it->second.get() : nullptr;
  }

  /**
   * @brief Return the library-wide shared mutex that protects the class
   *        registry.
   *
   * External code may acquire this lock when performing multi-step operations
   * on the class registry that must be atomic.  Use
   * `std::shared_lock<std::shared_mutex>` for read-only access and
   * `std::unique_lock<std::shared_mutex>` for mutations.
   *
   * @return Reference to the static `std::shared_mutex`.
   */
  static inline std::shared_mutex& getMutex() {
    static std::shared_mutex mutex;
    return mutex;
  }

  /**
   * @brief Return the global class registry.
   *
   * Maps class name (`key_t`) to the registered prototype (`shared_ptr<dynamic>`).
   * Access must be protected by `getMutex()`.
   *
   * @return Reference to the static `collection`.
   */
  static inline collection& getClasses() {
    static collection classes;
    return classes;
  }

 private:
  mutable std::map<key_t, field> fields_;
  mutable std::map<key_t, method> methods_;
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
 * - `dynamic_ptr(key_t, map<key_t, field>&&)` — directly constructs the
 *   managed `dynamic` with a class key and initial fields, matching the
 *   `dynamic` constructor signature.
 *
 * ### Example
 * @code{.cpp}
 * dynamic_ptr obj{"Point"_key, {{"x"_key, 0.0f}, {"y"_key, 0.0f}}};
 * obj->serialize(serializer(ss));
 * @endcode
 */
class dynamic_ptr : public std::shared_ptr<dynamic> {
 public:
  using std::shared_ptr<dynamic>::shared_ptr;
  using std::shared_ptr<dynamic>::operator=;

  /**
   * @brief Construct from an rvalue `dynamic`, taking ownership.
   *
   * @param that  The `dynamic` object to move into the heap-allocated managed
   *              object.
   */
  dynamic_ptr(dynamic&& that) {
    auto dyn = new dynamic{std::move(that)};
    *this = std::shared_ptr<dynamic>(dyn);
  }

  /**
   * @brief Construct a new `dynamic` in-place with a class key and fields.
   *
   * @param klass   Class name key (default 0 = anonymous).
   * @param fields  Initial named fields.
   */
  dynamic_ptr(key_t klass = 0U, std::map<key_t, field>&& fields = {}) {
    *this = std::shared_ptr<dynamic>(new dynamic{klass, std::move(fields)});
  }
};

inline void field::serialize(serializer& out) const {
  const field_base& fb = static_cast<const field_base&>(*this);
  out.write(static_cast<unsigned char>(fb.index()));
  std::visit(
      [&out](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          // nothing to write for an empty field
        } else if constexpr (std::is_same_v<T, std::shared_ptr<dynamic>>) {
          out.write(v != nullptr);
          if (v != nullptr) {
            v->serialize(out);
          }
        } else {
          // covers hash_t, key_t, bool, int32_t, float,
          //         std::string, std::vector<bool/int32_t/float>
          out.write(v);
        }
      },
      fb);
}

inline field field::deserialize(deserializer& in) {
  // Type tags match the variant index order in field_base:
  //  0=monostate  1=hash_t  2=key_t  3=bool  4=int32_t  5=float
  //  6=shared_ptr<dynamic>  7=string  8=vector<bool>
  //  9=vector<int32_t>  10=vector<float>
  const auto type = in.read<unsigned char>();
  switch (type) {
    case 0: return field{std::monostate{}};
    case 1: return field{in.read<hash_t>()};
    case 2: return field{in.read<key_t>()};
    case 3: return field{in.read<bool>()};
    case 4: return field{in.read<int32_t>()};
    case 5: return field{in.read<float>()};
    case 6: {
      if (!in.read<bool>()) return field{std::shared_ptr<dynamic>{}};
      return field{dynamic::deserialize(in)};
    }
    case 7: {
      std::string s;
      in.read(s);
      return field{std::move(s)};
    }
    case 8: {
      std::vector<bool> v;
      in.read(v);
      return field{std::move(v)};
    }
    case 9: {
      std::vector<int32_t> v;
      in.read(v);
      return field{std::move(v)};
    }
    case 10: {
      std::vector<float> v;
      in.read(v);
      return field{std::move(v)};
    }
    default:
      throw std::runtime_error("Not implemented");
  }
}

inline void dynamic::serialize(serializer& out) const {
  out.write(fields_.size());
  for (auto& field : fields_) {
    out.write(field.first);
    field.second.serialize(out);
  }
}

inline void dynamic::serializeWithTemplate(serializer& out) const {
  auto& classes = getClasses();
  auto klass = as<key_t>(CLASS);
  auto it = classes.find(klass);

  out.write(klass);
  while (it != classes.end()) {
    for (const auto& kv : it->second->fields_) {
      // Use the instance's own value for this field when available;
      // otherwise fall back to the prototype's default value.
      auto instance_it = fields_.find(kv.first);
      if (instance_it != fields_.end()) {
        instance_it->second.serialize(out);
      } else {
        kv.second.serialize(out);
      }
    }
    klass = it->second->as<key_t>(PARENT);
    it = classes.find(klass);
  }
}

inline std::shared_ptr<dynamic> dynamic::deserialize(deserializer& in) {
  auto dyn = std::shared_ptr<dynamic>(new dynamic{});
  auto count = in.read<size_t>();
  for (size_t i = 0; i < count; ++i) {
    auto key = in.read<key_t>();
    dyn->fields_[key] = field::deserialize(in);
  }

  return dyn;
}

inline std::shared_ptr<dynamic> dynamic::deserializeWithTemplate(
    deserializer& in) {
  auto dyn = std::shared_ptr<dynamic>(new dynamic{});
  auto klass = in.read<key_t>();
  auto& classes = getClasses();
  auto it = classes.find(klass);

  while (it != classes.end()) {
    for (const auto& kv : it->second->fields_) {
      dyn->fields_[kv.first] = field::deserialize(in);
    }
    klass = it->second->as<key_t>(PARENT);
    it = classes.find(klass);
  }

  return dyn;
}

/**
 * @namespace bdg::bison::extensions
 * @brief Optional extensions that add interoperability with other formats.
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
std::shared_ptr<dynamic> from_json(std::string json);
} // namespace extensions

} // namespace bdg::bison
