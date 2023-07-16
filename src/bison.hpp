#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <variant>

namespace bdg {
namespace bison {

using hash_t = int32_t;

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

class node;

using method = std::function<node(node& /*self*/, const node& /*params*/)>;
using field = std::variant<
    std::monostate,
    key_t,
    bool,
    int32_t,
    float,
    std::string,
    node,
    std::vector<bool>,
    std::vector<int32_t>,
    std::vector<float>,
    std::vector<std::string>>;

class node {
 public:
  node(key_t klass = ""_key, std::map<key_t, field>&& fields = {});
  node(const node& that) = default;
  node(node&& that) noexcept = default;
  node& operator=(const node& that) = delete;
  node& operator=(node&& that) = default;
  virtual ~node();

  node clone() const;
  size_t size() const;
  bool erase(size_t pos);
  void clear();

  field& operator[](size_t pos);
  const field& operator[](size_t pos) const;

  field& operator[](key_t name);
  const field& operator[](key_t name) const;

  template <typename T>
  T& as(key_t name, T def = T{}) {
    auto& field = fields_[name];
    if (std::holds_alternative<std::monostate>(field)) {
      field = def;
    } else if (!std::holds_alternative<T>(field)) {
      throw std::runtime_error("Invalid type");
    }
    return std::get<T>(field);
  }

  template <typename T>
  const T& as(key_t name, T def = T{}) const {
    auto& field = fields_[name];
    if (std::holds_alternative<std::monostate>(field)) {
      field = def;
    } else if (!std::holds_alternative<T>(field)) {
      throw std::runtime_error("Invalid type");
    }
    return std::get<T>(field);
  }

  bool addField(key_t name, field value);
  bool addMethod(key_t name, method fn);
  node call(key_t name, const node& params);

  static bool addClass(const key_t parent, node&& klass);

 private:
  mutable std::map<key_t, field> fields_;
  mutable std::map<key_t, method> methods_;

  field* findField(key_t name) const;
  method* findMethod(key_t name) const;
  node* findClass(key_t name) const;

  static inline std::mutex& getMutex() {
    static std::mutex mutex;
    return mutex;
  }

  static inline std::unordered_map<key_t, node, key_t, key_t>& getClasses() {
    static std::unordered_map<key_t, node, key_t, key_t> classes;
    return classes;
  }
};

} // namespace bison
} // namespace bdg