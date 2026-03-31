// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

#pragma once

#include "src/core/bison_common.hpp"
#include "src/core/bison_serialization.hpp"
#include "src/core/bison_sync.hpp"

namespace bdg::bison {

class attribute {
 public:
  virtual ~attribute() = default;
};

template <typename T, typename... Args>
std::shared_ptr<const attribute> attr(Args&&... args) {
  static_assert(
      std::is_base_of_v<attribute, T>, "T must derive from attribute");
  return std::make_shared<T>(std::forward<Args>(args)...);
}

class field : public field_base {
 public:
  friend class dynamic;

  template <typename T>
  static auto to_field_value(T&& value) {
    using value_type = std::decay_t<T>;
    if constexpr (
        std::is_same_v<value_type, char*> ||
        std::is_same_v<value_type, const char*>) {
      return std::string(value);
    } else if constexpr (std::is_same_v<value_type, std::shared_ptr<dynamic>>) {
      return dynamic_ptr(std::forward<T>(value));
    } else {
      return std::forward<T>(value);
    }
  }

  field() : field_base(std::monostate{}) {}

  template <typename T, typename... Attrs>
  field(T value, Attrs&&... attrs)
      : field_base(to_field_value(std::forward<T>(value))),
        attributes_{std::forward<Attrs>(attrs)...} {}

  operator dynamic_ptr() const {
    return as<dynamic_ptr>();
  }

  operator std::string() const {
    return as<std::string>();
  }

  operator const std::string&() const {
    return as<std::string>();
  }

  template <typename T>
  operator T() const {
    if (!std::holds_alternative<T>(static_cast<const field_base&>(*this))) {
      throw std::runtime_error("Invalid type");
    }
    return std::get<T>(static_cast<const field_base&>(*this));
  }

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

  template <typename T>
  bool is() const {
    return std::holds_alternative<T>(static_cast<const field_base&>(*this));
  }

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

  template <typename T>
  const T& as(T def = T{}) const {
    if (!std::holds_alternative<T>(static_cast<const field_base&>(*this))) {
      throw std::runtime_error("Invalid type");
    }
    return std::get<T>(static_cast<const field_base&>(*this));
  }

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

  template <typename T>
  constexpr static unsigned char tag_of() {
    constexpr std::size_t idx = index_of<T>();
    static_assert(idx < 256, "field_base alternative count exceeds tag range");
    return static_cast<unsigned char>(idx);
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

  inline void serialize(stream_serializer& out) const;
  inline void serialize(buffer_serializer& out) const;
  inline static field deserialize(stream_deserializer& in);
  inline static field deserialize(buffer_deserializer& in);

 private:
  mutable std::vector<std::shared_ptr<const attribute>> attributes_;
};

class dynamic {
 public:
  friend class field;
  friend class dynamic_ptr;

  static inline constexpr hash_t CLASS = "__class"_key;
  static inline constexpr hash_t PARENT = "__parent"_key;

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

  inline dynamic clone() const {
    return dynamic(static_cast<const dynamic>(*this));
  }

  inline size_t size() const {
    auto it = fields_.lower_bound(key_t{0x80000000u});
    if (it == fields_.begin())
      return 0;
    --it;
    return static_cast<size_t>(it->first.id + 1);
  }

  inline bool erase(size_t pos) {
    return fields_.erase(static_cast<hash_t>(pos)) != 0;
  }

  inline bool empty() const {
    return fields_.empty();
  }

  inline void clear() {
    fields_.erase(fields_.begin(), fields_.lower_bound(key_t{0x80000000u}));
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

  inline void serialize(stream_serializer& out) const;
  inline void serialize(buffer_serializer& out) const;
  inline void serializeWithSchema(stream_serializer& out) const;
  inline void serializeWithSchema(buffer_serializer& out) const;
  inline static dynamic deserialize(stream_deserializer& in);
  inline static dynamic deserialize(buffer_deserializer& in);
  inline static dynamic deserializeWithSchema(stream_deserializer& in);
  inline static dynamic deserializeWithSchema(buffer_deserializer& in);

  inline bool addField(key_t name, field value) {
    return fields_.emplace(std::make_pair(name, std::move(value))).second;
  }

  inline bool addMethod(key_t name, method fn) {
    return methods_.emplace(std::make_pair(name, fn)).second;
  }

  inline void setUserdata(std::shared_ptr<userdata> userdata) {
    userdata_ = std::move(userdata);
  }

  inline std::shared_ptr<userdata> getUserdata() const {
    return userdata_;
  }

  inline dynamic call(key_t name, const dynamic& params) {
    auto fn = findMethod(name);
    if (fn == nullptr) {
      throw std::runtime_error("Method not found");
    }
    return (*fn)(*this, params);
  }

  static dynamic instantiate(const key_t klass) {
    return dynamic{klass};
  }

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

  field* findField(key_t name) const {
    auto it = fields_.find(name);
    if (it == fields_.end()) {
      auto lp = getRegistry().rlock();
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

  method* findMethod(key_t name) const {
    auto it = methods_.find(name);
    if (it == methods_.end()) {
      auto lp = getRegistry().rlock();
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

  template <typename F>
  void forEach(F&& fn) const {
    for (const auto& kv : fields_) {
      fn(kv.first, kv.second);
    }
  }

  static inline synchronized<collection>& getRegistry() {
    static synchronized<collection> registry;
    return registry;
  }

 private:
  mutable std::map<key_t, field> fields_;
  mutable std::unordered_map<key_t, method, key_t, key_t> methods_;
  mutable std::shared_ptr<userdata> userdata_;
};

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

namespace extensions {

dynamic_ptr from_json(std::string json);
dynamic_ptr from_yaml(std::string yaml);

} // namespace extensions

} // namespace bdg::bison
