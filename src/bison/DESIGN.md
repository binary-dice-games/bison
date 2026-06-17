# Bison Core Library Design

## 1. Scope

This document describes the design of the core Bison library implemented in src/bison. It covers:

- Runtime object model in C++ (dynamic, field, dynamic_ptr)
- Binary serialization and deserialization format
- Class registry and inheritance lookup behavior
- JSON and YAML import extensions
- Stable C ABI facade (bison_c)
- Concurrency, ownership, and error handling contracts

Implementation files:

- bison.hpp: main C++ API and most inline implementation
- bison.cpp: JSON/YAML extension implementation
- bison_c.h: public C ABI
- bison_c.cpp: C ABI implementation and bridging logic

## 2. Design Goals

The core library is designed to provide:

1. A dynamic, schema-light runtime object model
2. Compact, endian-stable binary encoding
3. Optional schema-aware compact encoding through registered classes
4. Runtime method dispatch on dynamic objects
5. Interoperability with C callers and higher-level language bindings
6. Predictable behavior for inheritance and lookup without generated code

Non-goals of core:

- Network transport or RPC concerns
- Strong compile-time type schemas
- Full thread safety for concurrent mutation of a single object instance

## 3. Namespace and Public Surface

Primary namespace: bdg::bison

Major public types:

- hash_t, key_t, _key literal
- field (variant wrapper plus attributes)
- dynamic (runtime object)
- dynamic_ptr (convenience shared_ptr wrapper)
- stream_serializer and stream_deserializer
- buffer_serializer and buffer_deserializer
- userdata and attribute base classes
- extensions::from_json and extensions::from_yaml

C ABI surface is exported through bison_c.h as opaque bison_handle APIs.

## 4. Data Model

### 4.1 Keys and Name Resolution

Field and class names are represented by key_t (wrapping a 32-bit hash_t) produced by:

- constexpr hash(const char*) using FNV-1a style hashing
- constexpr operator""_key for compile-time hashing

Named keys use high-bit-set hash values, while numeric array-style indices are plain small integers. This allows one map to represent both object-like and array-like content.

### 4.2 field Value Space

field extends a std::variant named field_base with these alternatives:

- monostate
- hash_t
- key_t
- bool
- int32_t
- float
- shared_ptr<dynamic>
- string
- vector<bool>
- vector<int32_t>
- vector<float>

Key behavior:

- Type-checked assignment: a non-empty field cannot change to another type
- as<T>() typed access with optional default initialization for empty fields
- Implicit conversions for string and shared_ptr<dynamic>
- Optional metadata attributes attached per field (not serialized)

### 4.3 dynamic Object

dynamic is the central runtime object with:

- fields_: map<key_t, field>
- methods_: unordered_map<key_t, method>
- userdata_: shared_ptr<userdata>

Reserved field keys:

- CLASS ("__class")
- PARENT ("__parent")
- NAMESPACE ("__namespace") — set automatically by addClass; 0U = global namespace

Object supports both:

- Named field access via key hashes
- Numeric index access for sequence-like usage

## 5. Class Registry and Inheritance

### 5.1 Namespace Registry (Multipleton)

The class registry is a **namespace-partitioned multipleton** rather than a flat singleton.

- Type: `unordered_map<key_t, collection>` (called `namespace_map`)
  - Outer key: namespace hash (`0U` = global/default namespace)
  - Value: `collection` = `unordered_map<key_t, shared_ptr<dynamic>>` (class name → prototype)
- Accessor: `dynamic::getRegistry()` returns a `synchronized<namespace_map>&`

**Namespace key**: produced by the `"name"_key` literal or `hash("name")`.  The value `0U` (hash 0) is reserved for the global (default) namespace and is never a valid FNV-1a hash of any string.

### 5.2 Class Registration

`dynamic::addClass(ns, klass, parent)` — registers in namespace `ns`.
Use `ns = 0U` (empty/global namespace) for the global namespace.

Registration actions:
- Writes `parent` to `klass[PARENT]`.
- Writes `ns` to `klass[NAMESPACE]`.
- Cycle detection is performed **within the same namespace only**.
- Rejects duplicate class names **within the same namespace**.

Two classes with the same name may coexist in different namespaces without collision.

Reserved field keys (set automatically on class prototypes):

| Key | Constant | Purpose |
|---|---|---|
| `"__class"` | `CLASS` | Hash of the class name |
| `"__parent"` | `PARENT` | Hash of the parent class name |
| `"__namespace"` | `NAMESPACE` | Hash of the registered namespace |

### 5.3 Instantiation

`dynamic::instantiate(klass)` — creates an instance in the global namespace.
`dynamic::instantiate(ns, klass)` — creates an instance and sets `__namespace = ns`.

When `ns != 0`, the `__namespace` field is written to the new instance immediately so that the first field/method/class lookup goes directly to the correct namespace collection.

### 5.4 Namespace Resolution during Lookup

When `findField`, `findMethod`, or `findClass` is called, the private helper `resolveNamespace` determines the correct namespace collection:

1. If the instance already has `__namespace` in its `fields_`, that value is returned directly (O(1) fast path).
2. Otherwise, absent `__namespace` means the global namespace (`0U`).
3. The resolved namespace is cached into `fields_[NAMESPACE]` for future calls.

### 5.5 Inheritance Lookup Rules

- Field lookup first checks instance `fields_`.
- If not found, the correct namespace collection is selected and the class chain is traversed (via `PARENT` links, all within the same namespace collection).
- On hit, the field value is copied into the instance's `fields_` cache.
- Method lookup uses the same strategy with `methods_` cache.

Implication:

- First inherited read is more expensive; subsequent reads are fast.
- Instance can override inherited members by writing local entries.

## 6. Method Dispatch Model

Method type:

- function<dynamic(dynamic& self, const dynamic& params)>

Registration:

- addMethod(name, fn) returns false on duplicate method name

Invocation:

- call(name, params) resolves method via local map then class chain
- Throws runtime_error if method is not found
- self is mutable, enabling object-state updates during method call

## 7. Serialization Design

### 7.1 Endianness

Wire encoding is big-endian (network order).

- byte_swap swaps on little-endian hosts and is a no-op on big-endian hosts
- Scalars are swapped on write and read

### 7.2 Serializer Backends

Two equivalent serializer families:

- stream_serializer and stream_deserializer for std::istream/std::ostream
- buffer_serializer and buffer_deserializer for in-memory byte buffers

Both support:

- Scalar read/write
- size-prefixed string and vector read/write
- raw byte read/write

### 7.3 field Binary Format

Serialized field layout:

1. One-byte type tag (variant index)
2. Encoded value payload by type

Tag mapping:

- 0 monostate
- 1 hash_t
- 2 key_t
- 3 bool
- 4 int32_t
- 5 float
- 6 shared_ptr<dynamic> as bool-present flag plus nested dynamic payload
- 7 string
- 8 vector<bool>
- 9 vector<int32_t>
- 10 vector<float>

Attributes are intentionally not serialized.

### 7.4 dynamic Standard Format (Self-Describing)

serialize and deserialize format:

1. size_t field count
2. For each field: key_t key, then serialized field value

Properties:

- Fully self-describing
- Does not require registered classes to decode
- Encodes all instance fields currently present

### 7.5 dynamic Template Format (Schema-Driven Compact)

serializeWithSchema and deserializeWithSchema format:

1. key_t namespace id
2. key_t class id
3. For each field declared in class prototype chain order: serialized field value only

Properties:

- More compact than standard mode (keys omitted)
- Requires class registry consistency between writer and reader (same namespace and class definitions)
- Uses prototype default field values when instance is missing a field during serialization
- Namespace and class are resolved from the instance at write time and stored at the head

## 8. JSON and YAML Extensions

Implemented in bison.cpp under bdg::bison::extensions.

### 8.1 JSON Import

from_json parses text using nlohmann::json and maps:

- null -> null shared_ptr<dynamic>
- bool -> bool
- integer/unsigned -> int32_t
- float -> float
- string -> string
- array -> dynamic with numeric indices
- object -> dynamic with hashed name keys

### 8.2 YAML Import

from_yaml parses text using libyaml event stream.

Plain scalar coercion order:

1. null aliases (null, ~, empty) -> null shared_ptr<dynamic>
2. true aliases (true, yes, on) -> bool true
3. false aliases (false, no, off) -> bool false
4. integer parse -> int32_t
5. floating parse -> float
6. fallback -> string

Mappings become dynamic objects with hashed keys; sequences become index-based dynamic objects.

## 9. C ABI Design

### 9.1 Handle Representation and Ownership

bison_handle is opaque in C, implemented as heap-allocated shared_ptr<dynamic> in C++.

Ownership model:

- create and instantiate return owning handle (refcount +1)
- add_ref clones shared_ptr handle (shares same object)
- release deletes handle and decrements refcount

### 9.2 Error Model

C APIs return:

- BISON_OK on success
- Negative bison_error for failures
- NULL for handle-returning failures

All C++ exceptions are caught at ABI boundary and translated to error codes.

### 9.3 C API Capability Areas

- Object lifecycle and reference counting
- JSON/YAML import wrappers
- Class registration (global: `bison_add_class`; named namespace: `bison_add_class_ns`)
- Object instantiation (global: `bison_instantiate`; named namespace: `bison_instantiate_ns`)
- Class lookup
- Scalar and object field set/get by key and by numeric index
- Method registration via callback bridge
- Method invocation
- Utility hashing function bison_key

### 9.4 Method Callback Bridge

C callback signature:

- fn(self, params, result, user)

Bridge behavior in bison_c.cpp:

- self is exposed via non-owning handle wrapper
- params is copied into a heap dynamic for callback safety
- result is a fresh dynamic object that callback populates
- wrapper converts callback output back to C++ dynamic return value

## 10. Concurrency Model

Thread-safe:

- Global class registry read/write synchronization through shared_mutex

Not thread-safe by default:

- Mutating fields and methods on the same dynamic instance from multiple threads
- Concurrent method registration and field mutation on the same object

Caller requirement:

- Synchronize access when sharing a mutable dynamic instance across threads

## 11. Performance Characteristics

Design choices that affect performance:

- Hashed keys avoid runtime string comparisons
- Inheritance lookups are cached into instance maps
- buffer_serializer/deserializer reduce stream virtual dispatch overhead
- Template serialization omits keys for better size and speed

Potential costs:

- map and variant operations for highly hot loops
- Dynamic allocations for nested object graphs and C handle wrappers
- First inherited lookup incurs registry traversal

## 12. Constraints and Edge Cases

- field enforces stable type once non-empty; type changes require explicit reset semantics
- Numeric and named keys share one map; callers should avoid accidental key collisions by using intended key spaces
- Template serialization depends on consistent class definitions and field order across processes
- JSON unsigned values are currently coerced to int32_t, so range truncation risk exists for large values

## 13. Integration Boundaries

Core library provides object and serialization primitives consumed by:

- Example and benchmark binaries
- C shared library for bindings
- Python, C#, and Java bindings via bison_c
- Planned RMI framework on top of dynamic objects and binary payloads

## 14. Recommended Evolution Areas

1. Explicit protocol version stamp for template serialization layout
2. Optional 64-bit numeric support in field_base for broader data compatibility
3. Optional immutable dynamic mode for lock-free read sharing patterns
4. Stronger error typing in C++ API (beyond runtime_error strings)
5. Optional CLASS restoration guarantee in template deserialization path

## 15. Summary

Bison core is a dynamic object runtime with two serialization modes, inheritance-aware field and method resolution, and a stable C ABI wrapper. The design balances flexibility (runtime object graph and callable methods) with portability (endian-safe binary format) and interoperability (JSON/YAML import and C API), making it a suitable foundation for higher-level systems such as the planned RMI layer.

