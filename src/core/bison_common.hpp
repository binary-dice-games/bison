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

namespace endian {
const size_t big = 1;
const size_t little = 0;
const size_t native = []() {
  uint32_t i = 0x01020304;
  return ((char*)&i)[0] == 0x04 ? little : big;
}();
} // namespace endian

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

  T result{};
  const size_t size = sizeof(T);
  for (size_t i = 0; i < size; ++i) {
    reinterpret_cast<unsigned char*>(&result)[size - i - 1] =
        reinterpret_cast<const unsigned char*>(&value)[i];
  }
  return result;
}

using hash_t = uint32_t;
using buffer = std::vector<uint8_t>;

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

constexpr hash_t operator""_key(const char* name, std::size_t size) noexcept {
  return hash(name);
}

struct _key_t {
  _key_t(hash_t v = 0) : id(v) {}
  _key_t(const char* input) : id(hash(input)) {}
  _key_t(const std::string& input) : id(hash(input.c_str())) {}
  operator hash_t() const {
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

using key_t = struct _key_t;

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

using method =
    std::function<dynamic(dynamic& /*self*/, const dynamic& /*params*/)>;

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

using collection = std::unordered_map<key_t, dynamic_ptr, key_t, key_t>;

} // namespace bdg::bison
