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

#include "src/bison/bison_common.hpp"
#include "src/bison/bison_serialization.hpp"
#include "src/bison/bison_sync.hpp"

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

// ── Built-in attribute types ──────────────────────────────────────────────────

/** @brief Human-readable display name for a field or class. */
class DisplayName : public attribute {
 public:
  explicit DisplayName(std::string name) : name_(std::move(name)) {}
  const std::string& name() const { return name_; }

 private:
  std::string name_;
};

/** @brief Human-readable description for a field or class. */
class Description : public attribute {
 public:
  explicit Description(std::string text) : text_(std::move(text)) {}
  const std::string& text() const { return text_; }

 private:
  std::string text_;
};

/** @brief Logical category grouping for a field or class. */
class Category : public attribute {
 public:
  explicit Category(std::string name) : name_(std::move(name)) {}
  const std::string& name() const { return name_; }

 private:
  std::string name_;
};

/**
 * @brief Marks a field or class as obsolete with an optional explanatory
 *        message.
 */
class Obsolete : public attribute {
 public:
  explicit Obsolete(std::string message = {}) : message_(std::move(message)) {}
  const std::string& message() const { return message_; }

 private:
  std::string message_;
};

/** @brief Marks a field as required (must hold a non-empty value). */
class Required : public attribute {
 public:
  Required() = default;
};

/**
 * @brief Inclusive [min, max] range hint for a numeric field.
 *
 * Consumers (e.g. property editors, validation layers) may use this to clamp
 * input or render an appropriate widget.  This is advisory only — the runtime
 * does not enforce the range.
 */
class Range : public attribute {
 public:
  Range(double min, double max) : min_(min), max_(max) {}
  double min() const { return min_; }
  double max() const { return max_; }

 private:
  double min_;
  double max_;
};

/**
 * @brief Increment step hint for a numeric field.
 *
 * Advisory increment used by property editors and slider widgets.  Does not
 * affect serialisation or runtime assignment.
 */
class Step : public attribute {
 public:
  explicit Step(double step) : step_(step) {}
  double step() const { return step_; }

 private:
  double step_;
};

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
    } else if constexpr (
        std::is_constructible_v<std::shared_ptr<dynamic>, value_type> &&
        !std::is_same_v<value_type, std::shared_ptr<dynamic>>) {
      // shared_ptr<T> where T derives from dynamic (e.g. a typed ui subclass).
      // Upcast to dynamic_ptr so the field stores the canonical pointer type.
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
    return cast_value<std::string>(static_cast<const field_base&>(*this));
  }

  operator const std::string&() const {
    return as<std::string>();
  }

  template <typename T>
  operator T() const {
    return cast_value<T>(static_cast<const field_base&>(*this));
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
   * @brief Convert the field value to @p T, casting between compatible types.
   *
   * Unlike `as<T>()`, this method performs cross-type conversions:
   * numeric types cast to one another via `static_cast`, arithmetic types
   * convert to `std::string` via `std::to_string` (bools give `"true"` /
   * `"false"`), and strings parse to numeric types via `std::stoi` /
   * `std::stof`.  Throws `std::runtime_error` for incompatible conversions
   * and re-throws `std::invalid_argument` / `std::out_of_range` from
   * string-to-numeric parsing.
   *
   * @tparam T  Target type.
   * @return Value converted to @p T.
   */
  template <typename T>
  T get_as() const {
    return cast_value<T>(static_cast<const field_base&>(*this));
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
      static_cast<field_base&>(*this) = std::move(def);
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

  /** @brief Attach an additional attribute to this field. */
  void addAttribute(std::shared_ptr<const attribute> a) {
    attributes_.push_back(std::move(a));
  }

  inline void serialize(stream_serializer& out) const;
  inline void serialize(buffer_serializer& out) const;
  inline static field deserialize(stream_deserializer& in);
  inline static field deserialize(buffer_deserializer& in);

 private:
  // ── Cross-type cast helper ──────────────────────────────────────────────
  template <typename To>
  static To cast_value(const field_base& base) {
    return std::visit(
        [](const auto& v) -> To {
          using From = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<From, To>) {
            return v;
          } else if constexpr (std::is_same_v<From, std::monostate>) {
            throw std::runtime_error("Field is empty");
          } else if constexpr (std::is_arithmetic_v<From> &&
                               std::is_arithmetic_v<To>) {
            return static_cast<To>(v);
          } else if constexpr (std::is_same_v<To, std::string>) {
            if constexpr (std::is_same_v<From, bool>)
              return v ? "true" : "false";
            else if constexpr (std::is_arithmetic_v<From>)
              return std::to_string(v);
            else
              throw std::runtime_error("Incompatible types");
          } else if constexpr (std::is_same_v<From, std::string>) {
            if constexpr (std::is_integral_v<To>)
              return static_cast<To>(std::stoi(v));
            else if constexpr (std::is_floating_point_v<To>)
              return static_cast<To>(std::stof(v));
            else
              throw std::runtime_error("Incompatible types");
          } else {
            throw std::runtime_error("Incompatible types");
          }
        },
        base);
  }

  mutable std::vector<std::shared_ptr<const attribute>> attributes_;
};

/**
 * @brief A callable method with optional attribute annotations.
 *
 * Parallel to `field` for named callable members on a `dynamic` object.
 * Holds a `method_fn` plus zero or more `attribute` tags that describe
 * the method's purpose, category, and lifecycle state.
 *
 * `call()` is declared here and defined after `dynamic` to avoid a
 * forward-declaration issue with the return type.
 */
class method {
 public:
  friend class dynamic;

  /**
   * @brief Construct a method with a callable and zero or more attribute args.
   *
   * @tparam Attrs  Zero or more `std::shared_ptr<const attribute>` values.
   * @param  fn     Method implementation callable.
   * @param  attrs  Attribute annotations forwarded into the internal vector.
   */
  template <
      typename... Attrs,
      std::enable_if_t<
          (... && std::is_convertible_v<
                      std::decay_t<Attrs>,
                      std::shared_ptr<const attribute>>),
          int> = 0>
  explicit method(method_fn fn, Attrs&&... attrs)
      : fn_(std::move(fn)), attrs_{std::forward<Attrs>(attrs)...} {}

  /**
   * @brief Construct a method with a callable and a pre-built attribute vector.
   *
   * @param fn     Method implementation callable.
   * @param attrs  Attribute vector (moved in).
   */
  method(method_fn fn, std::vector<std::shared_ptr<const attribute>> attrs)
      : fn_(std::move(fn)), attrs_(std::move(attrs)) {}

  /**
   * @brief Construct a method with input/output param specs and attribute args.
   *
   * @tparam Attrs   Zero or more `std::shared_ptr<const attribute>` values.
   * @param  fn      Method implementation callable.
   * @param  input   Annotated dynamic describing input parameter fields.
   * @param  output  Annotated dynamic describing output parameter fields.
   * @param  attrs   Attribute annotations for the method itself.
   */
  template <typename... Attrs>
  method(
      method_fn fn,
      dynamic_ptr input,
      dynamic_ptr output,
      Attrs&&... attrs)
      : fn_(std::move(fn)),
        input_(std::move(input)),
        output_(std::move(output)),
        attrs_{std::forward<Attrs>(attrs)...} {}

  /** @brief Return input parameter spec, or nullptr if none was registered. */
  const dynamic* inputSpec() const { return input_.get(); }

  /** @brief Return output parameter spec, or nullptr if none was registered. */
  const dynamic* outputSpec() const { return output_.get(); }

  /**
   * @brief Invoke this method.
   *
   * @param self    Mutable reference to the calling object.
   * @param params  Read-only argument object.
   * @return The `dynamic` value returned by the implementation.
   */
  dynamic call(dynamic& self, const dynamic& params) const;

  /**
   * @brief Find and return a pointer to the first attached attribute of type
   *        @p T, or `nullptr` if none is present.
   *
   * @tparam T  A type derived from `attribute`.
   * @return Const pointer to the attribute, or `nullptr`.
   */
  template <typename T>
  const T* findAttribute() const {
    static_assert(
        std::is_base_of_v<attribute, T>, "T must derive from attribute");
    for (const auto& a : attrs_)
      if (const T* p = dynamic_cast<const T*>(a.get()))
        return p;
    return nullptr;
  }

  /** @brief Attach an additional attribute to this method. */
  void addAttribute(std::shared_ptr<const attribute> a) {
    attrs_.push_back(std::move(a));
  }

 private:
  method_fn fn_;
  dynamic_ptr input_;
  dynamic_ptr output_;
  std::vector<std::shared_ptr<const attribute>> attrs_;
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

  /**
   * @brief Return a deep copy of this object.
   *
   * All fields are copied; nested `dynamic_ptr` fields are recursively cloned
   * so the result shares no mutable state with the original.  Methods are
   * copied by value (their `method_fn` callables are shared, which is safe
   * since they are immutable).  The `userdata` pointer is shallow-copied.
   */
  inline dynamic clone() const {
    dynamic copy{static_cast<const dynamic&>(*this)};
    for (auto& kv : copy.fields_) {
      if (kv.second.is<dynamic_ptr>()) {
        auto& ptr = kv.second.as<dynamic_ptr>();
        if (ptr != nullptr)
          ptr = dynamic_ptr{std::make_shared<dynamic>(ptr->clone())};
      }
    }
    return copy;
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

  /// @brief Callable type stored in registered class prototypes.
  using factory_fn = std::function<dynamic_ptr()>;

  /**
   * @brief Return a factory that creates a fresh @p T instance with the given
   *        class keys.
   *
   * Convenience helper for the factory-aware `addClass` overload; eliminates
   * the need for typed subclasses to write their own factory lambdas.
   *
   * Example:
   * ```cpp
   * dynamic::addClass("wish"_key, proto,
   *     dynamic::make_factory<import_handler>("wish"_key, "__WishImport"_key));
   * ```
   *
   * @tparam T     Concrete subclass of `dynamic`.
   * @param  ns    Namespace key passed to `instantiate<T>`.
   * @param  klass Class key passed to `instantiate<T>`.
   */
  template <typename T, typename = std::enable_if_t<std::is_base_of_v<dynamic, T>>>
  static factory_fn make_factory(key_t ns, key_t klass) {
    return [ns, klass]() -> dynamic_ptr {
      return dynamic::instantiate<T>(ns, klass);
    };
  }

  /**
   * @brief Instantiate a registered class by namespace and class key.
   *
   * Looks up the registered prototype for @p klass in @p ns, copies its
   * factory under the registry read lock, releases the lock, then calls the
   * factory.  If no factory was registered with `addClass`, falls back to
   * constructing a plain `dynamic` — correct for prototype-only classes such as
   * UI elements.
   *
   * @param ns    Namespace key; `0U` for the global namespace.
   * @param klass Class key.
   * @return New instance, or an empty `dynamic_ptr` if the class is not found.
   */
  static dynamic_ptr create_instance(key_t ns, key_t klass) {
    factory_fn factory;
    {
      auto lp = getRegistry().rlock();
      auto ns_it = lp->find(ns);
      if (ns_it == lp->end()) return {};
      auto cls_it = ns_it->second.find(klass);
      if (cls_it == ns_it->second.end()) return {};
      factory = cls_it->second->factory_;
    }
    if (factory) return factory();
    return dynamic_ptr{new dynamic(dynamic::instantiate(ns, klass))};
  }

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
   * @param m     Method instance (implementation + optional attributes).
   * @return `true` if the method was registered; `false` if the key was
   *         already taken.
   */
  inline bool addMethod(key_t name, method m) {
    return methods_.emplace(name, std::move(m)).second;
  }

  /**
   * @brief Iterate over all methods registered directly on this object.
   *
   * @tparam F  Callable with signature `void(key_t, const method&)`.
   * @param  fn  Visitor invoked for each `(key, method)` pair.
   */
  template <typename F>
  void forEachMethod(F&& fn) const {
    for (const auto& kv : methods_) {
      fn(kv.first, kv.second);
    }
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
    auto* m = findMethod(name);
    if (m == nullptr) {
      throw std::runtime_error("Method not found");
    }
    return m->call(*this, params);
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
   * @brief Create an instance of a `dynamic` subclass @p T in a namespace.
   *
   * Calls the non-template `instantiate(ns, klass)` to build the base object
   * and moves it into a heap-allocated @p T via `T(dynamic&&)`.
   *
   * @tparam T      Concrete subclass of `dynamic` with a `T(dynamic&&)` ctor.
   * @param  ns     Hash of the namespace; `0U` for the global namespace.
   * @param  klass  Hash of the class name.
   * @return `std::shared_ptr<T>` owning the new instance.
   */
  template <typename T,
            typename = std::enable_if_t<std::is_base_of_v<dynamic, T>>>
  static std::shared_ptr<T> instantiate(const key_t ns, const key_t klass) {
    return std::make_shared<T>(dynamic::instantiate(ns, klass));
  }

  /**
   * @brief Create an instance of a `dynamic` subclass @p T in the global
   *        namespace.
   * @tparam T     Concrete subclass of `dynamic` with a `T(dynamic&&)` ctor.
   * @param  klass Hash of the class name.
   * @return `std::shared_ptr<T>` owning the new instance.
   */
  template <typename T,
            typename = std::enable_if_t<std::is_base_of_v<dynamic, T>>>
  static std::shared_ptr<T> instantiate(const key_t klass) {
    return instantiate<T>(key_t{0U}, klass);
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
  /**
   * @brief Register a class prototype with class-level attribute annotations.
   *
   * Attaches each attribute in @p class_attrs to the `CLASS` field of @p klass
   * before delegating to the base `addClass` overload.  The attributes are
   * surfaced by `handle_describe` as class-level metadata.
   *
   * @param ns          Namespace to register in; `0U` for the global namespace.
   * @param klass       Prototype object; its `CLASS` field must be set.
   * @param parent      Hash of the parent class name (`0U` for a root class).
   * @param class_attrs Attributes to attach to the class (e.g. `DisplayName`).
   * @return `true` on success, `false` on duplicate or cycle.
   */
  static bool addClass(
      const key_t ns,
      dynamic_ptr klass,
      const key_t parent,
      std::vector<std::shared_ptr<const attribute>> class_attrs) {
    for (auto& a : class_attrs)
      (*klass)[CLASS].addAttribute(std::move(a));
    return addClass(ns, std::move(klass), parent);
  }

  /**
   * @brief Register a typed subclass prototype without an explicit cast.
   *
   * Convenience overload for callers that already hold a `std::shared_ptr<T>`
   * (where `T` is a `dynamic` subclass).  The pointer is implicitly narrowed to
   * `dynamic_ptr` before forwarding to the primary overload.
   *
   * @tparam T          Subclass of `dynamic`.
   * @param  ns         Namespace to register in.
   * @param  klass      Prototype as a typed shared pointer.
   * @param  parent     Parent class name hash (`0U` for root).
   * @param  class_attrs Optional class-level attributes.
   */
  template <typename T,
            typename = std::enable_if_t<std::is_base_of_v<dynamic, T>>>
  static bool addClass(
      const key_t ns,
      std::shared_ptr<T> klass,
      const key_t parent = key_t{0U},
      std::vector<std::shared_ptr<const attribute>> class_attrs = {}) {
    return addClass(ns, dynamic_ptr{std::move(klass)}, parent,
                    std::move(class_attrs));
  }

  /**
   * @brief Register @p klass with an instance factory.
   *
   * @p factory is stored in the prototype so that `create_instance` returns
   * an object of the correct concrete type without virtual dispatch.  All
   * other behaviour (inheritance chain, namespace, duplicate detection) is
   * identical to the zero-factory overload.
   *
   * @p factory comes last (after @p parent) so that calls with explicit parent
   * are unambiguous with the zero-factory overload.  Use `key_t{0U}` for
   * @p parent when there is no parent class.
   *
   * @param ns      Namespace key.
   * @param klass   Prototype for the class being registered.
   * @param parent  Parent class key; `key_t{0U}` for no parent.
   * @param factory Callable that returns a freshly constructed instance.
   * @return `true` on success, `false` on duplicate or cycle.
   */
  static bool addClass(key_t ns, dynamic_ptr klass, key_t parent,
                       factory_fn factory) {
    klass->factory_ = std::move(factory);
    return addClass(ns, std::move(klass), parent);
  }

  static bool
  addClass(const key_t ns, dynamic_ptr klass, const key_t parent = key_t{0U}) {
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
  /**
   * @brief Find a method on this instance or its class prototype chain.
   *
   * Returns a pointer to the method (caching into `methods_` on first
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
   * @brief Iterate over every `dynamic_ptr` child field cast to type @p T.
   *
   * Filters `fields_` to entries whose value is a `dynamic_ptr`, then attempts
   * a `dynamic_cast` to `T*`.  Only children that succeed the cast invoke @p fn.
   * Fields that are not `dynamic_ptr`, or whose pointed-to object is not a @p T,
   * are silently skipped.
   *
   * @tparam T  Concrete subclass of `dynamic`.
   * @tparam F  Callable with signature `void(key_t, T&)`.
   * @param  fn Visitor invoked for each matching child.
   */
  template <typename T, typename F,
            typename = std::enable_if_t<std::is_base_of_v<dynamic, T>>>
  void forEachChild(F&& fn) const {
    for (const auto& kv : fields_) {
      if (kv.second.is<dynamic_ptr>()) {
        const auto& ptr = kv.second.as<dynamic_ptr>();
        if (auto* typed = dynamic_cast<T*>(ptr.get())) {
          fn(kv.first, *typed);
        }
      }
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
  factory_fn factory_;  // set once by the factory-aware addClass; empty → plain-dynamic fallback

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

inline dynamic method::call(dynamic& self, const dynamic& params) const {
  return fn_(self, params);
}

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

/**
 * @brief Serialize @p d to a JSON string.
 *
 * Field-value types are mapped as follows: null/monostate → JSON `null`,
 * `bool` → JSON boolean, `int32_t` → JSON integer, `float` → JSON float,
 * `std::string` → JSON string, `dynamic_ptr` → nested JSON object,
 * numeric vectors → JSON arrays, `std::vector<uint8_t>` → lowercase hex
 * string, `hash_t`/`key_t` → resolved name or `"#<decimal>"`.
 *
 * Field keys are resolved via @p keys (a hash-ID → name map).  Because
 * `key_t` is a one-way FNV-1a hash, keys absent from the map are emitted
 * as `"#<decimal>"`.
 *
 * @param d       Source object.
 * @param keys    Hash-ID to display-name map used to resolve field keys.
 * @param indent  Indentation width in spaces; pass -1 for compact output.
 * @return UTF-8 JSON string representation of @p d.
 */
std::string to_json(const dynamic& d,
                    const std::unordered_map<uint32_t, std::string>& keys = {},
                    int indent = 2);

/**
 * @brief Serialize @p d to a YAML string.
 *
 * Produces block-style YAML.  Field-value types are mapped as follows:
 * null/monostate → `null`, `bool` → `true`/`false`, `int32_t` → integer,
 * `float` → decimal (with trailing `.0` when needed to distinguish from
 * integers), `std::string` → double-quoted string, `dynamic_ptr` → nested
 * YAML mapping, numeric vectors → YAML sequences, `std::vector<uint8_t>` →
 * double-quoted lowercase hex string, `hash_t`/`key_t` → resolved name or
 * `"#<decimal>"`.
 *
 * Field key resolution follows the same rules as `to_json`.
 *
 * @param d     Source object.
 * @param keys  Hash-ID to display-name map used to resolve field keys.
 * @return UTF-8 YAML string representation of @p d.
 * @throws std::runtime_error on internal libyaml emitter failure.
 */
std::string to_yaml(const dynamic& d,
                    const std::unordered_map<uint32_t, std::string>& keys = {});

} // namespace extensions

} // namespace bdg::bison
