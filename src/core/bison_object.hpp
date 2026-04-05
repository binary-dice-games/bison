// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file bison_object.hpp
 * @brief Core object model: `attribute`, `field`, and `dynamic`.
 *
 * This header defines the three central building blocks of the Bison runtime:
 * - `attribute` – abstract tag type for optional per-field metadata.
 * - `field` – a typed variant value with optional attributes and type-checked
 *   assignment semantics.
 * - `dynamic` – a runtime object with named/indexed fields, method dispatch,
 *   and prototype-chain inheritance via a namespace-partitioned class registry.
 *
 * Serialization method bodies for `field` and `dynamic` are inlined here
 * because they reference the serializer types defined in
 * `bison_serialization.hpp`.
 */

#pragma once

#include "src/core/bison_common.hpp"
#include "src/core/bison_serialization.hpp"
#include "src/core/bison_sync.hpp"

namespace bdg::bison {

/**
 * @brief Abstract base class for optional per-field metadata tags.
 *
 * Derive from `attribute` to attach arbitrary, non-serialised metadata to a
 * `field`.  Instances are always managed through `std::shared_ptr<const
 * attribute>` and are retrieved via `field::findAttribute<T>()`.
 */
class attribute {
 public:
  virtual ~attribute() = default;
};

/**
 * @brief Factory helper that constructs a heap-allocated `attribute` subtype.
 *
 * @tparam T     A type derived from `attribute`.
 * @tparam Args  Constructor argument types forwarded to `T`.
 * @param  args  Constructor arguments.
 * @return `std::shared_ptr<const attribute>` owning the new instance.
 */
template <typename T, typename... Args>
std::shared_ptr<const attribute> attr(Args&&... args) {
  static_assert(
      std::is_base_of_v<attribute, T>, "T must derive from attribute");
  return std::make_shared<T>(std::forward<Args>(args)...);
}

/**
 * @brief A typed variant value with optional metadata attributes.
 *
 * `field` extends `field_base` (a `std::variant`) and enforces *stable-type*
 * semantics: once a field is assigned a non-empty value its type is fixed and
 * cannot be changed to a different alternative.  Attempting to assign a
 * mismatched type throws `std::runtime_error`.
 *
 * Attributes (see `attribute`) can be attached at construction time for
 * out-of-band metadata; they are never serialised.
 */
class field : public field_base {
 public:
  friend class dynamic;

  /**
   * @brief Coerce an arbitrary value to the canonical `field_base` alternative.
   *
   * Raw `char*` / `const char*` are promoted to `std::string`;
   * `std::shared_ptr<dynamic>` is wrapped in `dynamic_ptr`.  All other types
   * are forwarded unchanged.
   *
   * @tparam T  Source value type.
   * @param  value  Value to coerce.
   * @return The coerced value in its canonical field type.
   */
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

  /** @brief Construct an empty (monostate) field. */
  field() : field_base(std::monostate{}) {}

  /**
   * @brief Construct a field with a value and optional attributes.
   *
   * @tparam T      Value type (coerced via `to_field_value`).
   * @tparam Attrs  Attribute types (`std::shared_ptr<const attribute>`).
   * @param  value  Initial value.
   * @param  attrs  Zero or more attributes to attach.
   */
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

  /**
   * @brief Type-safe assignment; throws `std::runtime_error` on type mismatch.
   *
   * If the field is currently empty (monostate) any type is accepted.
   * Otherwise the new value must be the same alternative as the current one.
   *
   * @tparam T  Type of the new value.
   * @param  value  New value to assign.
   * @return Reference to `*this`.
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
   * @brief Check whether the field currently holds a value of type @p T.
   *
   * @tparam T  Alternative type to test.
   * @return `true` if the active alternative is `T`.
   */
  template <typename T>
  bool is() const {
    return std::holds_alternative<T>(static_cast<const field_base&>(*this));
  }

  /**
   * @brief Return a mutable reference to the value as type @p T.
   *
   * If the field is empty it is default-initialised to @p def first.
   * Throws `std::runtime_error` if the active alternative is not @p T.
   *
   * @tparam T    Requested alternative type.
   * @param  def  Default value used to initialise an empty field.
   * @return Mutable reference to the stored `T` value.
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
   * @brief Return a const reference to the value as type @p T.
   *
   * Throws `std::runtime_error` if the active alternative is not @p T.
   *
   * @tparam T    Requested alternative type.
   * @param  def  Unused; kept for symmetry with the mutable overload.
   * @return Const reference to the stored `T` value.
   */
  template <typename T>
  const T& as(T def = T{}) const {
    if (!std::holds_alternative<T>(static_cast<const field_base&>(*this))) {
      throw std::runtime_error("Invalid type");
    }
    return std::get<T>(static_cast<const field_base&>(*this));
  }

  /**
   * @brief Compile-time index of type @p T within `field_base`.
   *
   * @tparam T      Alternative type to locate.
   * @tparam index  Internal recursion parameter; callers should omit it.
   * @return Zero-based variant index, or `std::variant_size_v<field_base>` if
   *         @p T is not an alternative.
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
   * @brief One-byte serialisation tag for alternative type @p T.
   *
   * This is the value written to the wire stream to identify the type of a
   * serialised field.  Asserts at compile time that the index fits in a
   * `unsigned char`.
   *
   * @tparam T  Alternative type.
   * @return Serialisation tag byte.
   */
  template <typename T>
  constexpr static unsigned char tag_of() {
    constexpr std::size_t idx = index_of<T>();
    static_assert(idx < 256, "field_base alternative count exceeds tag range");
    return static_cast<unsigned char>(idx);
  }

  /**
   * @brief Find and return a pointer to the first attached attribute of
   *        type @p T, or `nullptr` if none is present.
   *
   * @tparam T  A type derived from `attribute`.
   * @return Const pointer to the attribute, or `nullptr`.
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

  inline void serialize(stream_serializer& out) const;
  inline void serialize(buffer_serializer& out) const;
  inline static field deserialize(stream_deserializer& in);
  inline static field deserialize(buffer_deserializer& in);

 private:
  mutable std::vector<std::shared_ptr<const attribute>> attributes_;
};

/**
 * @brief Runtime dynamic object with named/indexed fields and method dispatch.
 *
 * A `dynamic` is a map of `key_t → field` entries plus an optional map of
 * named methods.  Three reserved field keys carry identity metadata:
 * - `CLASS` (`"__class"`) – hash of the object's registered class name.
 * - `PARENT` (`"__parent"`) – hash of the parent class name (set by
 *   `addClass`).
 * - `NAMESPACE` (`"__namespace"`) – hash of the namespace the class was
 *   registered in (set automatically by `addClass`; `0U` = global namespace).
 *
 * ### Namespaces
 * Classes are registered in namespaces via `addClass(ns, klass, parent)`.
 * Each namespace is an independent `collection` (class-name → prototype map)
 * inside a process-wide `namespace_map`.  The same class name may exist in
 * multiple namespaces without collision.  When performing field / method /
 * class lookups, the library reads the `__namespace` field of the instance to
 * select the correct collection. If `__namespace` is absent, the global
 * namespace (`0U`) is used and cached in `__namespace`.
 *
 * ### Inheritance
 * Field and method lookups walk the prototype chain stored in the namespace's
 * collection (see `getRegistry()`).  On the first inherited read the resolved
 * value is cached into the instance's own `fields_` map so that subsequent
 * accesses are O(1).
 *
 * ### Numeric vs named indices
 * Numeric indices (0, 1, 2, …) are stored as plain `hash_t` values below
 * `0x80000000`.  Hashed names always have the MSB set.  Both share the same
 * `fields_` map, so an object can behave as both a named record and a sequence
 * simultaneously.
 *
 * ### Thread safety
 * The global namespace registry is protected by a `shared_mutex`.  Mutations
 * to a single instance are **not** thread-safe; callers must synchronise
 * external concurrent access.
 */
class dynamic {
 public:
  friend class field;
  friend class dynamic_ptr;

  static inline constexpr hash_t CLASS =
      "__class"_key; /**< Reserved: class name hash. */
  static inline constexpr hash_t PARENT =
      "__parent"_key; /**< Reserved: parent class hash. */
  static inline constexpr hash_t NAMESPACE =
      "__namespace"_key; /**< Reserved: namespace hash (0 = global). */

  /**
   * @brief Construct a dynamic object with an optional class and initial
   * fields.
   *
   * @param klass     Hash of the class name; `0` for an anonymous object.
   * @param fields    Initial field map (moved in).
   * @param userdata  Optional application-defined userdata payload.
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

  /** @brief Return a deep copy of this object (fields and methods). */
  inline dynamic clone() const {
    return dynamic(static_cast<const dynamic>(*this));
  }

  /**
   * @brief Number of numeric-index entries (array-like size).
   *
   * Returns `last_index + 1` where `last_index` is the largest integer key
   * below the named-key threshold (`0x80000000`).
   */
  inline size_t size() const {
    auto it = fields_.lower_bound(key_t{0x80000000u});
    if (it == fields_.begin())
      return 0;
    --it;
    return static_cast<size_t>(it->first.id + 1);
  }

  /**
   * @brief Remove the field at numeric index @p pos.
   * @return `true` if the field existed and was erased.
   */
  inline bool erase(size_t pos) {
    return fields_.erase(static_cast<hash_t>(pos)) != 0;
  }

  /** @brief Return `true` if the object has no fields at all. */
  inline bool empty() const {
    return fields_.empty();
  }

  /** @brief Erase all numeric-index (array-like) fields; named fields remain.
   */
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
    auto f = findField(name);
    return f != nullptr ? *f : fields_[name];
  }

  inline const field& operator[](key_t name) const {
    auto f = findField(name);
    return f != nullptr ? *f : fields_[name];
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

  /**
   * @brief Add a field by name if it does not already exist.
   *
   * @param name   Hash key for the new field.
   * @param value  Field value to insert.
   * @return `true` if the field was inserted; `false` if the key was already
   *         present.
   */
  inline bool addField(key_t name, field value) {
    return fields_.emplace(std::make_pair(name, std::move(value))).second;
  }

  /**
   * @brief Register a callable method on this object.
   *
   * @param name  Hash key for the method.
   * @param fn    Method implementation (see `method` typedef).
   * @return `true` if the method was registered; `false` if the key was
   *         already taken.
   */
  inline bool addMethod(key_t name, method fn) {
    return methods_.emplace(std::make_pair(name, fn)).second;
  }

  /** @brief Attach or replace the userdata payload on this object. */
  inline void setUserdata(std::shared_ptr<userdata> userdata) {
    userdata_ = std::move(userdata);
  }

  /** @brief Return the userdata payload, or `nullptr` if none is attached. */
  inline std::shared_ptr<userdata> getUserdata() const {
    return userdata_;
  }

  /**
   * @brief Invoke a method by name.
   *
   * Resolves the method through the instance map then the class chain.
   *
   * @param name    Hash key of the method to call.
   * @param params  Read-only arguments object passed to the method.
   * @return The `dynamic` value returned by the method implementation.
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
   * @brief Create a new dynamic object in a specific namespace.
   *
   * @param ns     Hash of the namespace the class was registered in; `0U` for
   *               the global (default) namespace.  When non-zero, the
   *               `__namespace` field is set on the new instance so that
   *               field / method / class lookups target the correct namespace
   *               collection without an additional registry search.
   * @param klass  Hash of the class name.
   * @return A `dynamic` whose `CLASS` field is set to @p klass and, if @p ns
   *         is non-zero, whose `NAMESPACE` field is set to @p ns.
   */
  static dynamic instantiate(const key_t ns, const key_t klass) {
    dynamic obj{klass};
    if (ns.id != 0U) {
      obj.fields_[NAMESPACE] = ns;
    }
    return obj;
  }

  /**
   * @brief Create a new dynamic object in the global namespace.
   * @param klass  Hash of the class name.
   */
  static dynamic instantiate(const key_t klass) {
    return instantiate(key_t{0U}, klass);
  }

  /**
   * @brief Register a class prototype in the namespace registry.
   *
   * Sets the `PARENT` field of @p klass to @p parent, sets the `NAMESPACE`
   * field of @p klass to @p ns, and inserts the prototype into the registry
   * under `(ns, class_name)`.  Returns `false` if a class with the same name
   * is already registered in @p ns, or if registering would create a
   * circular inheritance chain within @p ns.
  *
  * @param ns      Hash of the namespace to register in; `0U` for the global
  *                (default) namespace.
  * @param klass   Prototype object; its `CLASS` field must be set.
  * @param parent  Hash of the parent class name (`0U` for a root class).
   * @return `true` on success, `false` on duplicate or cycle.
   */
  static bool addClass(
    const key_t ns,
    dynamic_ptr klass,
    const key_t parent = key_t{0U}) {
    auto name = klass->as<key_t>(CLASS);
    (*klass)[PARENT] = parent;
    (*klass)[NAMESPACE] = ns;

    auto lp = getRegistry().wlock();
    auto& col = (*lp)[ns]; // get or create the namespace's collection

    // Cycle detection: only traverse within the same namespace
    auto ancestor = parent;
    auto it = col.find(parent);
    while (it != col.end() && ancestor != name) {
      ancestor = it->second->as<key_t>(PARENT);
      it = col.find(ancestor);
    }

    if (ancestor == name) {
      return false;
    }

    return col.try_emplace(name, std::move(klass)).second;
  }

  /**
   * @brief Find a field on this instance or its class prototype chain.
   *
   * Returns a mutable pointer to the field (caching into `fields_` on first
   * inherited hit), or `nullptr` if not found anywhere in the chain.
   * The correct namespace collection is resolved via `resolveNamespace`.
   *
   * @param name  Hash key to look up.
   * @return Pointer to the resolved field, or `nullptr`.
   */
  field* findField(key_t name) const {
    // Fast path: field already cached in this instance.
    {
      auto it = fields_.find(name);
      if (it != fields_.end())
        return &it->second;
    }

    // Slow path: search the class prototype chain.
    {
      auto lp = getRegistry().rlock();
      // resolveNamespace may write __namespace into fields_, so we must
      // not rely on any iterator captured before this call.
      auto ns = resolveNamespace();
      auto nsIt = lp->find(ns);
      if (nsIt != lp->end()) {
        const auto& col = nsIt->second;
        auto classKey = as<key_t>(CLASS);
        for (auto itClass = col.find(classKey); itClass != col.end();) {
          auto& klass = itClass->second;
          auto itField = klass->fields_.find(name);
          if (itField != klass->fields_.end()) {
            fields_.insert(std::make_pair(name, itField->second));
            break;
          }
          itClass = col.find(klass->as<key_t>(PARENT));
        }
      }
    }

    // Return whatever is now in the cache (inserted above, or absent).
    auto it = fields_.find(name);
    return it != fields_.end() ? &it->second : nullptr;
  }

  /**
   * @brief Find a method on this instance or its class prototype chain.
   *
   * Returns a pointer to the callable (caching into `methods_` on first
   * inherited hit), or `nullptr` if not found anywhere in the chain.
   * The correct namespace collection is resolved via `resolveNamespace`.
   *
   * @param name  Hash key to look up.
   * @return Pointer to the resolved method, or `nullptr`.
   */
  method* findMethod(key_t name) const {
    // Fast path: method already cached in this instance.
    {
      auto it = methods_.find(name);
      if (it != methods_.end())
        return &it->second;
    }

    // Slow path: search the class prototype chain.
    {
      auto lp = getRegistry().rlock();
      auto ns = resolveNamespace();
      auto nsIt = lp->find(ns);
      if (nsIt != lp->end()) {
        const auto& col = nsIt->second;
        for (auto itClass = col.find(as<key_t>(CLASS)); itClass != col.end();) {
          auto& klass = itClass->second;
          auto itMethod = klass->methods_.find(name);
          if (itMethod != klass->methods_.end()) {
            methods_.insert(std::make_pair(name, itMethod->second));
            break;
          }
          itClass = col.find(klass->as<key_t>(PARENT));
        }
      }
    }

    auto it = methods_.find(name);
    return it != methods_.end() ? &it->second : nullptr;
  }

  /**
   * @brief Walk the class chain looking for a registered prototype named
   *        @p name.
   *
   * Uses the instance's namespace (resolved via `resolveNamespace`) to
   * select the correct collection before walking the chain.
   *
   * @param name  Hash of the class to search for.
   * @return Non-owning pointer to the prototype `dynamic`, or `nullptr` if
   *         @p name is not found in this object's ancestry.
   */
  dynamic* findClass(key_t name) const {
    auto lp = getRegistry().rlock();
    auto ns = resolveNamespace();
    auto nsIt = lp->find(ns);
    if (nsIt == lp->end())
      return nullptr;
    const auto& col = nsIt->second;
    auto klass = as<key_t>(CLASS);
    auto it = col.find(klass);
    while (it != col.end() && klass != name) {
      klass = it->second->as<key_t>(PARENT);
      it = col.find(klass);
    }

    return it != col.end() ? it->second.get() : nullptr;
  }

  /**
   * @brief Iterate over every field in insertion order.
   *
   * @tparam F  Callable with signature `void(key_t, const field&)`.
   * @param  fn  Visitor invoked for each `(key, field)` pair.
   */
  template <typename F>
  void forEach(F&& fn) const {
    for (const auto& kv : fields_) {
      fn(kv.first, kv.second);
    }
  }

  /**
   * @brief Return the process-wide namespace registry (thread-safe via
   *        `synchronized`).
   *
   * The registry is a `namespace_map`: an outer map keyed by namespace hash
   * (`0U` = global) whose values are `collection` maps (class name →
   * prototype `dynamic_ptr`).
   */
  static inline synchronized<namespace_map>& getRegistry() {
    static synchronized<namespace_map> registry;
    return registry;
  }

 private:
  mutable std::map<key_t, field> fields_;
  mutable std::unordered_map<key_t, method, key_t, key_t> methods_;
  mutable std::shared_ptr<userdata> userdata_;

  /**
   * @brief Resolve and cache the namespace for this instance's class chain.
   *
   * 1. If `__namespace` is already in `fields_`, it is returned immediately.
   * 2. If absent, the global namespace (`0U`) is cached and returned.
   *
   * This result is cached in `fields_[NAMESPACE]` so that subsequent calls
   * are O(1).  The write to `fields_` is safe because `fields_` is `mutable`
   * and single-instance mutations are documented as non-thread-safe.
   *
   * @return The namespace hash to use for prototype chain traversal.
   */
  key_t resolveNamespace() const {
    // 1. Return cached value if present.
    auto nsField = fields_.find(NAMESPACE);
    if (nsField != fields_.end() && nsField->second.is<key_t>()) {
      return nsField->second.as<key_t>();
    }

    // 2. Absent namespace means global namespace.
    fields_[NAMESPACE] = key_t{0U};
    return key_t{0U};
  }
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
  auto ns = in.read<key_t>();
  auto klass = in.read<key_t>();
  auto lp = getRegistry().rlock();
  auto nsIt = lp->find(ns);
  if (nsIt == lp->end())
    return dyn;
  const auto& col = nsIt->second;
  auto it = col.find(klass);

  while (it != col.end()) {
    for (const auto& kv : it->second->fields_) {
      dyn.fields_[kv.first] = field::deserialize(in);
    }
    klass = it->second->as<key_t>(PARENT);
    it = col.find(klass);
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
  auto ns = resolveNamespace();
  auto nsIt = lp->find(ns);

  out.write(ns);
  out.write(klass);
  if (nsIt == lp->end())
    return;
  const auto& col = nsIt->second;
  auto it = col.find(klass);
  while (it != col.end()) {
    for (const auto& kv : it->second->fields_) {
      auto instance_it = fields_.find(kv.first);
      if (instance_it != fields_.end()) {
        instance_it->second.serialize(out);
      } else {
        kv.second.serialize(out);
      }
    }
    klass = it->second->as<key_t>(PARENT);
    it = col.find(klass);
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
  auto ns = in.read<key_t>();
  auto klass = in.read<key_t>();
  auto lp = getRegistry().rlock();
  auto nsIt = lp->find(ns);
  if (nsIt == lp->end())
    return dyn;
  const auto& col = nsIt->second;
  auto it = col.find(klass);

  while (it != col.end()) {
    for (const auto& kv : it->second->fields_) {
      dyn.fields_[kv.first] = field::deserialize(in);
    }
    klass = it->second->as<key_t>(PARENT);
    it = col.find(klass);
  }

  return dyn;
}

namespace extensions {

/**
 * @brief Parse a JSON string and return the root object as a `dynamic_ptr`.
 *
 * JSON types are mapped as follows: `null` → null `shared_ptr<dynamic>`,
 * `bool` → `bool`, integers → `int32_t`, floats → `float`,
 * strings → `std::string`, arrays → index-keyed `dynamic`,
 * objects → hash-keyed `dynamic`.
 *
 * @param json  UTF-8 JSON text.
 * @return Root `dynamic_ptr` parsed from @p json.
 * @throws std::exception on parse error.
 */
dynamic_ptr from_json(std::string json);

/**
 * @brief Parse a YAML string and return the root object as a `dynamic_ptr`.
 *
 * Plain scalar coercion follows YAML 1.1 conventions (null aliases, boolean
 * aliases, integer, float, string).  Mappings become hash-keyed `dynamic`
 * objects; sequences become index-keyed `dynamic` objects.
 *
 * @param yaml  UTF-8 YAML text.
 * @return Root `dynamic_ptr` parsed from @p yaml.
 * @throws std::exception on parse error.
 */
dynamic_ptr from_yaml(std::string yaml);

} // namespace extensions

} // namespace bdg::bison
