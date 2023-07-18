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

namespace endian {
const size_t little = 0;
const size_t big = 1;
const size_t native = []() {
  uint32_t i = 0x01020304;
  return ((char*)&i)[0] == 0x04 ? little : big;
}();
} // namespace endian

template <typename T>
constexpr T byte_swap(T value) {
  if (endian::native == endian::little) {
    return value;
  } else {
    T result = 0;
    const size_t size = sizeof(T);
    for (size_t i = 0; i < size; ++i) {
      result = (result << 8) | ((value >> (i * 8)) & 0xFF);
    }
    return result;
  }
}

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

class field;
class dynamic;

using method =
    std::function<dynamic(dynamic& /*self*/, const dynamic& /*params*/)>;

using field_base = std::variant<
    std::monostate,
    key_t,
    bool,
    int32_t,
    float,
    std::shared_ptr<dynamic>,
    std::string,
    std::vector<bool>,
    std::vector<int32_t>,
    std::vector<float>,
    std::vector<std::string>>;

using collection =
    std::unordered_map<key_t, std::shared_ptr<dynamic>, key_t, key_t>;

class field : public field_base {
 public:
  using field_base::field_base;

  field(const char* text) : field_base(std::string{text}){};

  template <typename T>
  operator T() const {
    if (!std::holds_alternative<T>(*this)) {
      throw std::runtime_error("Invalid type");
    }
    return std::get<T>(*this);
  }

  template <typename T>
  T& as(T def = T{}) {
    if (std::holds_alternative<std::monostate>(*this)) {
      *this = def;
    } else if (!std::holds_alternative<T>(*this)) {
      throw std::runtime_error("Invalid type");
    }
    return std::get<T>(*this);
  }

  template <typename T>
  const T& as(T def = T{}) const {
    if (std::holds_alternative<std::monostate>(*this)) {
      *this = def;
    } else if (!std::holds_alternative<T>(*this)) {
      throw std::runtime_error("Invalid type");
    }
    return std::get<T>(*this);
  }
};

class dynamic {
 public:
  friend class dynamic_ptr;

  dynamic(dynamic&& that) noexcept = default;
  dynamic& operator=(const dynamic& that) = delete;
  dynamic& operator=(dynamic&& that) = default;
  virtual ~dynamic() {}

  inline dynamic clone() const {
    return dynamic(static_cast<const dynamic>(*this));
  }

  inline size_t size() const {
    auto it = fields_.rbegin();
    return it != fields_.rend() && it->first >= 0 ? (size_t)(it->first + 1) : 0;
  }

  inline bool erase(size_t pos) {
    return fields_.erase(static_cast<hash_t>(pos)) != 0;
  }

  inline void clear() {
    fields_.erase(fields_.lower_bound(0), fields_.end());
  }

  inline field& operator[](size_t pos) {
    return fields_[static_cast<hash_t>(pos)];
  }

  inline const field& operator[](size_t pos) const {
    return fields_[static_cast<hash_t>(pos)];
  }

  inline field& operator[](key_t name) {
    auto field = findField(name);
    return field != nullptr ? *field : fields_[name];
  }

  inline const field& operator[](key_t name) const {
    auto field = findField(name);
    return field != nullptr ? *field : fields_[name];
  }

  template <typename T>
  T& as(key_t name, T def = T{}) {
    auto& field = fields_[name];
    return field.as<T>();
  }

  template <typename T>
  const T& as(key_t name, T def = T{}) const {
    auto& field = fields_[name];
    return field.as<T>();
  }

  inline bool addField(key_t name, field value) {
    return fields_.emplace(std::make_pair(name, std::move(value))).second;
  }

  inline bool addMethod(key_t name, method fn) {
    return methods_.emplace(std::make_pair(name, fn)).second;
  }

  inline dynamic call(key_t name, const dynamic& params) {
    auto fn = findMethod(name);
    if (fn == nullptr) {
      throw std::runtime_error("Method not found");
    }
    return (*fn)(*this, params);
  }

  static bool addClass(const key_t parent, std::shared_ptr<dynamic> klass) {
    std::unique_lock<std::mutex> lk(getMutex());
    auto name = klass->as<key_t>("__class"_key);
    (*klass)["__parent"_key] = parent;

    auto ancestor = parent;
    auto& classes = getClasses();
    auto it = classes.find(parent);
    while (it != classes.end() && ancestor != name) {
      ancestor = it->second->as<key_t>("__parent"_key);
      it = classes.find(ancestor);
    }

    if (ancestor == name) {
      return false;
    }

    return classes.try_emplace(name, std::move(klass)).second;
  }

 private:
  mutable std::map<key_t, field> fields_;
  mutable std::map<key_t, method> methods_;

  dynamic(key_t klass = 0, std::map<key_t, field>&& fields = {})
      : fields_(std::move(fields)) {
    fields_["__class"_key] = klass;
  }

  dynamic(const dynamic& that) = default;

  field* findField(key_t name) const {
    auto it = fields_.find(name);
    if (it == fields_.end()) {
      std::unique_lock<std::mutex> lk(getMutex());
      auto& classes = getClasses();
      auto itClass = classes.find(as<key_t>("__parent"_key));
      while (itClass != classes.end() && it == fields_.end()) {
        auto& klass = itClass->second;
        auto itField = klass->fields_.find(name);
        if (itField != klass->fields_.end()) {
          it = fields_.insert(std::make_pair(name, itField->second)).first;
        } else {
          itClass = classes.find(klass->as<key_t>("__parent"_key));
        }
      }
    }

    return it != fields_.end() ? &it->second : nullptr;
  }

  method* findMethod(key_t name) const {
    auto it = methods_.find(name);
    if (it == methods_.end()) {
      std::unique_lock<std::mutex> lk(getMutex());
      auto& classes = getClasses();
      auto itClass = classes.find(as<key_t>("__parent"_key));
      while (itClass != classes.end() && it == methods_.end()) {
        auto& klass = itClass->second;
        auto itMethod = klass->methods_.find(name);
        if (itMethod != klass->methods_.end()) {
          it = methods_.insert(std::make_pair(name, itMethod->second)).first;
        } else {
          itClass = classes.find(klass->as<key_t>("__parent"_key));
        }
      }
    }

    return it != methods_.end() ? &it->second : nullptr;
  }

  dynamic* findClass(key_t name) const {
    std::unique_lock<std::mutex> lk(getMutex());
    auto& classes = getClasses();
    auto klass = as<key_t>("__class"_key);
    auto it = classes.find(klass);
    while (it != classes.end() && klass != name) {
      klass = it->second->as<key_t>("__parent"_key);
      it = classes.find(klass);
    }

    return it != classes.end() ? it->second.get() : nullptr;
  }

  static inline std::mutex& getMutex() {
    static std::mutex mutex;
    return mutex;
  }

  static inline collection& getClasses() {
    static collection classes;
    return classes;
  }
};

class dynamic_ptr : public std::shared_ptr<dynamic> {
 public:
  using std::shared_ptr<dynamic>::shared_ptr;

  dynamic_ptr(dynamic&& that) {
    auto dyn = new dynamic{std::move(that)};
    *this = std::shared_ptr<dynamic>(dyn);
  }

  dynamic_ptr(
      key_t klass = 0,
      std::initializer_list<std::pair<key_t, field>> params = {}) {
    std::map<key_t, field> fields;
    for (auto& it : params) {
      fields[it.first] = it.second;
    }
    *this = std::shared_ptr<dynamic>(new dynamic{klass, std::move(fields)});
  }

  //inline field& operator[](size_t pos) {
  //  return (*this)->operator[](static_cast<hash_t>(pos));
  //}

  //inline const field& operator[](size_t pos) const {
  //  return (*this)->operator[](static_cast<hash_t>(pos));
  //}

  //inline field& operator[](key_t name) {
  //  auto field = (*this)->findField(name);
  //  return field != nullptr ? *field : (*this)->operator[](name);
  //}

  //inline const field& operator[](key_t name) const {
  //  auto field = (*this)->findField(name);
  //  return field != nullptr ? *field : (*this)->operator[](name);
  //}

  inline dynamic_ptr serialize(std::ostream& out) {
    out.write("abc", 3);
    return *this;
  }

  inline dynamic_ptr deserialize(std::istream& in) {
    *this = dynamic_ptr{"class"_key, {{"example"_key, "example"}}};
    return *this;
  }
};

namespace extensions {
std::shared_ptr<dynamic> from_json(std::string json);
} // namespace extensions

} // namespace bison
} // namespace bdg