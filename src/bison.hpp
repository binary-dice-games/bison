#pragma once

#include <variant>
#include <functional>
#include <map>
#include <string>
#include <atomic>
#include <chrono>
#include <iostream>

namespace bdg {
namespace bison {

using hash_t = int32_t;

constexpr hash_t hash(const char *input)
{
  hash_t value = sizeof(hash_t) == 8 ? 0xcbf29ce484222325 : 0x811c9dc5;
  hash_t mask = sizeof(hash_t) == 8 ? 0x8FFFFFFFFFFFFFFF : 0x8FFFFFFF;
  const hash_t prime = sizeof(hash_t) == 8 ? 0x00000100000001b3 : 0x01000193;

  while (*input) {
    value ^= static_cast<hash_t>(*input);
    value *= prime;
    ++input;
  }

  return value & mask;
}

constexpr hash_t operator""_key(const char *name, std::size_t size) noexcept
{
  return hash(name);
}

struct _key_t {
  _key_t(hash_t v = 0) : id(v) {}
  operator hash_t() const { return id; }
  std::size_t operator()(const struct _key_t &k) const { return 0; }
  bool operator()(const struct _key_t &lhs, const struct _key_t &rhs) const
  {
    return false;
  }
  hash_t id;
};

using key_t = struct _key_t;

class node;

using method = std::function<node(node & /*self*/, const node & /*params*/)>;
using field =
  std::variant<std::monostate, hash_t, bool, int32_t, float, std::string, node,
               std::vector<bool>, std::vector<int32_t>, std::vector<float>,
               std::vector<std::string>>;

class node {
  public:
  node(hash_t klass = ""_key, std::map<hash_t, field> &&fields = {});
  node(const node &that) = default;
  node(node &&that) noexcept = default;
  node &operator=(const node &that) = delete;
  node &operator=(node &&that) = default;
  virtual ~node();

  node clone() const;
  size_t size() const;
  bool erase(size_t pos);

  field &operator[](size_t pos);
  const field &operator[](size_t pos) const;

  field &operator[](hash_t name);
  const field &operator[](hash_t name) const;

  template<typename T> node &with(hash_t name, T value)
  {
    fields_[name] = std::move(value);
    return *this;
  }

  template<typename T> T &as(hash_t name, T def = T{})
  {
    auto &field = fields_[name];
    if (std::holds_alternative<std::monostate>(field)) {
      field = def;
    } else if (!std::holds_alternative<T>(field)) {
      throw std::runtime_error("Invalid type");
    }
    return std::get<T>(field);
  }

  template<typename T> const T &as(hash_t name, T def = T{}) const
  {
    auto &field = fields_[name];
    if (std::holds_alternative<std::monostate>(field)) {
      field = def;
    } else if (!std::holds_alternative<T>(field)) {
      throw std::runtime_error("Invalid type");
    }
    return std::get<T>(field);
  }

  bool addMethod(hash_t name, method fn);
  node call(hash_t name, const node &params);

  static bool addClass(const hash_t name, const hash_t parent, node &&klass);

  private:
  mutable std::map<hash_t, field> fields_;
  mutable std::map<hash_t, method> methods_;

  static std::atomic<int32_t> watchdog_;
  static std::unordered_map<hash_t, node, hash_t, hash_t> classes_;
};

//node make_node(std::initializer_list<std::pair<hash_t, field>> initList);

}
}