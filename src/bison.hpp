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

class serializer;
class deserializer;
class attribute;
class field;
class dynamic;
class dynamic_ptr;

namespace endian {
const size_t default = 1;
const size_t little = 0;
const size_t big = 1;
const size_t native = []() {
  uint32_t i = 0x01020304;
  return ((char*)&i)[0] == 0x04 ? little : big;
}();
} // namespace endian

template <typename T>
constexpr T byte_swap(T value) {
  if (endian::native == endian::default) {
    return value;
  } else {
    T result = 0;
    const size_t size = sizeof(T);
    for (size_t i = 0; i < size; ++i) {
      ((unsigned char*)&result)[size - i - 1] = ((unsigned char*)&value)[i];
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
    std::vector<float>>;

using collection =
    std::unordered_map<key_t, std::shared_ptr<dynamic>, key_t, key_t>;

class serializer {
 public:
  serializer(std::ostream& out) : out_(out) {}
  serializer(const serializer& that) = delete;
  serializer(serializer&& that) = delete;

  template <typename T>
  serializer& write(T data) {
    data = byte_swap(data);
    out_.write(reinterpret_cast<const char*>(&data), sizeof(T));
    return *this;
  }

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

  serializer& write(const std::string& data) {
    size_t count = byte_swap(data.size());
    out_.write(reinterpret_cast<const char*>(&count), sizeof(size_t));
    out_.write(data.data(), data.size());
    return *this;
  }

  serializer& write(const char* data, std::streamsize count) {
    out_.write(data, count);
    return *this;
  }

 private:
  std::ostream& out_;
};

class deserializer {
 public:
  deserializer(std::istream& in) : in_(in) {}
  deserializer(const deserializer& that) = delete;
  deserializer(deserializer&& that) = delete;

  template <typename T>
  T read() {
    T data{};
    in_.read(reinterpret_cast<char*>(&data), sizeof(T));
    data = byte_swap(data);
    return data;
  }

  template <typename T>
  deserializer& read(T& data) {
    in_.read(reinterpret_cast<char*>(&data), sizeof(T));
    data = byte_swap(data);
    return *this;
  }

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

  deserializer& read(std::string& data) {
    size_t count = 0;
    in_.read(reinterpret_cast<char*>(&count), sizeof(size_t));
    count = byte_swap(count);
    data.resize(count);
    in_.read(data.data(), data.size());
    return *this;
  }

  deserializer& read(char* data, std::streamsize count) {
    in_.read(data, count);
    return *this;
  }

 private:
  std::istream& in_;
};

class attribute {};

template <typename T, typename... Args>
std::shared_ptr<const attribute> attr(Args&&... args) {
  static_assert(
      std::is_base_of_v<attribute, T>, "T must derive from attribute");
  return std::make_shared<T>(std::forward<Args>(args)...);
}

class field : public field_base {
 public:
  friend class dynamic;

  field() : field_base(std::monostate{}) {}

  template <typename T, typename... Attrs>
  field(T value, Attrs&&... attrs)
      : field_base(value), attributes_{std::forward<Attrs>(attrs)...} {}

  template <typename T>
  operator T() const {
    if (!std::holds_alternative<T>(*this)) {
      throw std::runtime_error("Invalid type");
    }
    return std::get<T>(*this);
  }

  template <typename T>
  field& operator=(const T& value) {
    if (std::holds_alternative<std::monostate>(*this)) {
      field_base::operator=(value);
    } else if (!std::holds_alternative<T>(*this)) {
      throw std::runtime_error("Invalid type");
    } else {
      field_base::operator=(value);
    }
    return *this;
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

  inline void serialize(serializer& out) const;
  inline static field deserialize(deserializer& in);

 private:
  mutable std::vector<std::shared_ptr<const attribute>> attributes_;
};

class dynamic {
 public:
  friend class field;
  friend class dynamic_ptr;

  static inline const key_t CLASS = "__class"_key;
  static inline const key_t PARENT = "__parent"_key;

  dynamic(key_t klass = 0, std::map<key_t, field>&& fields = {})
      : fields_(std::move(fields)) {
    fields_[CLASS] = klass;
  }

  dynamic(const dynamic& that) = default;
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

  field& at(key_t name) {
    return (*this)[name];
  }

  const field& at(key_t name) const {
    return (*this)[name];
  }

  field& at(size_t pos) {
    return (*this)[pos];
  }

  const field& at(size_t pos) const {
    return (*this)[pos];
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

  inline void serialize(serializer& out) const;
  inline static std::shared_ptr<dynamic> deserialize(deserializer& in);

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
    std::unique_lock<std::recursive_mutex> lk(getMutex());
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

  field* findField(key_t name) const {
    auto it = fields_.find(name);
    if (it == fields_.end()) {
      std::unique_lock<std::recursive_mutex> lk(getMutex());
      auto& classes = getClasses();
      auto itClass = classes.find(as<key_t>(PARENT));
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

  method* findMethod(key_t name) const {
    auto it = methods_.find(name);
    if (it == methods_.end()) {
      std::unique_lock<std::recursive_mutex> lk(getMutex());
      auto& classes = getClasses();
      auto itClass = classes.find(as<key_t>(PARENT));
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

  dynamic* findClass(key_t name) const {
    std::unique_lock<std::recursive_mutex> lk(getMutex());
    auto& classes = getClasses();
    auto klass = as<key_t>(CLASS);
    auto it = classes.find(klass);
    while (it != classes.end() && klass != name) {
      klass = it->second->as<key_t>(PARENT);
      it = classes.find(klass);
    }

    return it != classes.end() ? it->second.get() : nullptr;
  }

  static inline std::recursive_mutex& getMutex() {
    static std::recursive_mutex mutex;
    return mutex;
  }

  static inline collection& getClasses() {
    static collection classes;
    return classes;
  }

 private:
  mutable std::map<key_t, field> fields_;
  mutable std::map<key_t, method> methods_;
};

class dynamic_ptr : public std::shared_ptr<dynamic> {
 public:
  using std::shared_ptr<dynamic>::shared_ptr;

  dynamic_ptr(dynamic&& that) {
    auto dyn = new dynamic{std::move(that)};
    *this = std::shared_ptr<dynamic>(dyn);
  }

  dynamic_ptr(key_t klass = 0, std::map<key_t, field>&& fields = {}) {
    *this = std::shared_ptr<dynamic>(new dynamic{klass, std::move(fields)});
  }
};

inline void field::serialize(serializer& out) const {
  out.write(static_cast<unsigned char>(index()));
  switch (index()) {
    case field::index_of<std::monostate>(): {
    } break;
    case field::index_of<key_t>(): {
      out.write(std::get<key_t>(*this));
    } break;
    case field::index_of<bool>(): {
      out.write(std::get<bool>(*this));
    } break;
    case field::index_of<int32_t>(): {
      out.write(std::get<int32_t>(*this));
    } break;
    case field::index_of<float>(): {
      out.write(std::get<float>(*this));
    } break;
    case field::index_of<std::shared_ptr<dynamic>>(): {
      auto& dyn = std::get<std::shared_ptr<dynamic>>(*this);
      out.write(dyn != nullptr);
      if (dyn != nullptr) {
        dyn->serialize(out);
      }
    } break;
    case field::index_of<std::string>(): {
      out.write(std::get<std::string>(*this));
    } break;
    case field::index_of<std::vector<bool>>(): {
      out.write(std::get<std::vector<bool>>(*this));
    } break;
    case field::index_of<std::vector<int32_t>>(): {
      out.write(std::get<std::vector<int32_t>>(*this));
    } break;
    case field::index_of<std::vector<float>>(): {
      out.write(std::get<std::vector<float>>(*this));
    } break;
    default:
      throw std::runtime_error("Not implemented");
  }
}

inline field field::deserialize(deserializer& in) {
  field field{};
  auto type = in.read<unsigned char>();
  switch (type) {
    case field::index_of<std::monostate>(): {
      field = std::monostate{};
    } break;
    case field::index_of<key_t>(): {
      field = in.read<key_t>();
    } break;
    case field::index_of<bool>(): {
      field = in.read<bool>();
    } break;
    case field::index_of<int32_t>(): {
      field = in.read<int32_t>();
    } break;
    case field::index_of<float>(): {
      field = in.read<float>();
    } break;
    case field::index_of<std::shared_ptr<dynamic>>(): {
      field = std::shared_ptr<dynamic>{};
      if (in.read<bool>()) {
        auto value = std::shared_ptr<dynamic>(new dynamic{});
        value->deserialize(in);
        field = value;
      }
    } break;
    case field::index_of<std::string>(): {
      field = in.read<std::string>();
    } break;
    case field::index_of<std::vector<bool>>(): {
      field = std::vector<bool>{};
      in.read(std::get<std::vector<bool>>(field));
    } break;
    case field::index_of<std::vector<int32_t>>(): {
      field = std::vector<int32_t>{};
      in.read(std::get<std::vector<int32_t>>(field));
    } break;
    case field::index_of<std::vector<float>>(): {
      field = std::vector<float>{};
      in.read(std::get<std::vector<float>>(field));
    } break;
    default:
      throw std::runtime_error("Not implemented");
  }
  return field;
}

inline void dynamic::serialize(serializer& out) const {
  out.write(fields_.size());
  for (auto& field : fields_) {
    out.write(field.first);
    field.second.serialize(out);
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

namespace extensions {
std::shared_ptr<dynamic> from_json(std::string json);
} // namespace extensions

} // namespace bison
} // namespace bdg