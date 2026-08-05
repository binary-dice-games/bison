/**
 * @file bison_c.h
 * @brief Pure-C public API for the Bison dynamic-object library.
 *
 * This header exposes Bison as a multiplatform shared library (`.dll` on
 * Windows, `.dylib` on macOS, `.so` on Linux) with a stable C ABI.  It is
 * safe to `#include` from C or C++ projects alike.
 *
 * ## Object lifecycle
 * Every `bison_handle` is an opaque, reference-counted token that wraps a
 * `std::shared_ptr<bdg::bison::dynamic>`.  The reference count starts at 1
 * when the object is created.
 *
 * | Function | Effect on ref-count |
 * |---|---|
 * | `bison_create`, `bison_from_json`, `bison_from_yaml` | +1 (caller owns) |
 * | `bison_add_ref` | +1 (returns new handle sharing the same object) |
 * | `bison_release` | −1 (object destroyed when count reaches 0) |
 * | `bison_to_json`, `bison_to_yaml` | no change (read-only serialization) |
 *
 * Every handle obtained from a creation or `bison_add_ref` call **must** be
 * released with `bison_release` exactly once.
 *
 * ## Error handling
 * Functions that can fail return `BISON_OK` on success or a negative
 * `bison_error` code on failure.  Functions that return a `bison_handle`
 * return `NULL` on failure.  The library never throws exceptions across the C
 * boundary; all C++ exceptions are caught and converted to error codes.
 *
 * ## Thread safety
 * The library's global class registry is protected by a shared mutex (the same
 * one used internally).  Multiple threads may call read-only functions
 * concurrently.  Mutations to individual `bison_handle` objects are **not**
 * thread-safe; callers must synchronise access to the same handle.
 *
 * ## Platforms
 * `BISON_API` is defined as the correct symbol-visibility attribute for each
 * platform:
 * - **Windows** (`_WIN32`)  : `__declspec(dllexport)` when building the DLL,
 *                             `__declspec(dllimport)` when consuming it.
 * - **GCC / Clang** with `-fvisibility=hidden` :
 * `__attribute__((visibility("default")))`.
 * - **All others** : empty (no annotation needed).
 *
 * ### Minimal usage example (C)
 * @code{.c}
 * #include "bison_c.h"
 *
 * bison_handle h = bison_create(0);
 * bison_set_int(h, bison_key("score"), 42);
 * int32_t v = 0;
 * bison_get_int(h, bison_key("score"), &v);   // v == 42
 * bison_release(h);
 * @endcode
 */

#ifndef BISON_C_H
#define BISON_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Platform visibility macros ─────────────────────────────────────────── */

#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef BISON_BUILDING_DLL
#define BISON_API __declspec(dllexport)
#else
#define BISON_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define BISON_API __attribute__((visibility("default")))
#else
#define BISON_API
#endif

/* ─── Opaque handles ─────────────────────────────────────────────────────── */

/**
 * @brief Opaque handle that represents a reference-counted `dynamic` object.
 *
 * Never dereference the pointer directly; always use the `bison_*` API
 * functions.  A null `bison_handle` (value `NULL`) indicates an invalid or
 * error state.
 */
typedef struct bison_handle_* bison_handle;

/**
 * @brief Hash type used for pre-hashed names in the C API.
 *
 * Values are produced by `bison_key()` and consumed by APIs that accept
 * hashed class identifiers and field names.
 */
typedef uint32_t bison_hash;

/* ─── Error codes ────────────────────────────────────────────────────────── */

/**
 * @brief Return codes used by all mutating Bison C API functions.
 *
 * Zero (`BISON_OK`) always means success.  Negative values are errors.
 */
typedef enum bison_error {
  BISON_OK = 0, /**< Success. */
  BISON_ERR_NULL = -1, /**< A required handle or pointer argument was NULL. */
  BISON_ERR_TYPE = -2, /**< The field holds a different type than requested. */
  BISON_ERR_NOT_FOUND = -3, /**< Method or field not found. */
  BISON_ERR_DUPLICATE = -4, /**< Attempted to add a duplicate class or method. */
  BISON_ERR_EXCEPTION = -5, /**< An unexpected C++ exception was caught. */
  BISON_ERR_PARSE = -6, /**< Input failed to parse (JSON / YAML text, or a `bison_deserialize()` buffer). */
} bison_error;

/* ─── C method callback type ─────────────────────────────────────────────── */

/**
 * @brief Function-pointer type for methods registered on dynamic objects.
 *
 * The callback receives:
 * @param self    The object on which the method is being called (mutable).
 * @param params  A separate object containing call arguments (read-only).
 * @param result  Output handle for the return value; the library allocates a
 *                new object; the callee must populate it and must **not**
 *                release @p result (the library takes ownership).
 * @param user    User-defined context pointer passed to `bison_add_method`.
 */
typedef void (*bison_method_fn)(bison_handle self, bison_handle params, bison_handle result, void* user);

/* ═══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ═════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Create a new, empty dynamic object.
 *
 * @param klass_name  Hashed name of the class (use `bison_key()` to compute);
 *                    pass `0` for an anonymous object.
 * @return New handle (ref-count 1) or `NULL` on allocation failure.
 *
 * @code{.c}
 * bison_handle h = bison_create(0);
 * // ... use h ...
 * bison_release(h);
 * @endcode
 */
BISON_API bison_handle bison_create(bison_hash klass_name);

/**
 * @brief Create a new dynamic object by class key using C++
 * `dynamic::create_instance`.
 *
 * This creates a new instance of the requested class in the specified
 * namespace.  If a factory was registered for the class (`addClass(...,
 * factory)` -- the usual case for a class backed by a C++ subclass, e.g. an
 * application's own content types), the factory is used, so the result is
 * the real registered subtype rather than a plain `dynamic` slice; a
 * `dynamic_cast<T*>` a caller performs on the underlying instance (via
 * whatever mechanism inspects it) sees the actual type. Falls back to a
 * plain `dynamic` -- unconditionally, even if `ns_name`/`klass_name` names
 * no registered class at all -- when no factory applies, matching
 * `dynamic::instantiate()`'s original always-succeeds behavior for an
 * anonymous or unregistered class (e.g. `bison_instantiate(0, 0)`). Pass
 * `0` for `ns_name` to use the global (default) namespace.
 *
 * @param ns_name     Hash of the namespace name (use `bison_key()`); pass
 *                    `0` for the global (default) namespace.
 * @param klass_name  Hashed class name (use `bison_key()` to compute).
 * @return New handle (ref-count 1) or `NULL` on allocation failure.
 */
BISON_API bison_handle bison_instantiate(bison_hash ns_name, bison_hash klass_name);

/**
 * @brief Increment the reference count of @p h and return a new handle.
 *
 * Both @p h and the returned handle refer to the **same** underlying object.
 * Each must be released independently.
 *
 * @param h  A valid non-null handle.
 * @return A new handle sharing ownership, or `NULL` if @p h is `NULL`.
 */
BISON_API bison_handle bison_add_ref(bison_handle h);

/**
 * @brief Release a handle and decrement the reference count.
 *
 * When the reference count drops to zero the underlying `dynamic` object is
 * destroyed.  @p h must not be used after calling this function.
 *
 * @param h  Handle to release.  A `NULL` @p h is silently ignored.
 */
BISON_API void bison_release(bison_handle h);

/**
 * @brief Perform a deep clone of @p h and return the result as a new handle.
 *
 * All fields are copied.  Nested `dynamic` object fields are recursively
 * cloned, so the returned object shares no mutable state with @p h.
 * The caller owns the returned handle and must release it with
 * `bison_release`.
 *
 * @param h  Source object handle.
 * @return New handle (ref-count 1) owning the cloned object, or `NULL` if
 *         @p h is `NULL` or an allocation error occurs.
 */
BISON_API bison_handle bison_clone(bison_handle h);

/* ═══════════════════════════════════════════════════════════════════════════
 * Import helpers
 * ═════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Parse a JSON string and return the root object as a handle.
 *
 * @param json  Null-terminated UTF-8 JSON string.
 * @return New handle (ref-count 1), or `NULL` on parse error.
 *
 * @code{.c}
 * bison_handle h = bison_from_json("{\"x\": 1}");
 * int32_t x = 0;
 * bison_get_int(h, bison_key("x"), &x);  // x == 1
 * bison_release(h);
 * @endcode
 */
BISON_API bison_handle bison_from_json(const char* json);

/**
 * @brief Parse a YAML string and return the root object as a handle.
 *
 * Uses the same type-coercion rules as `extensions::from_yaml` in the C++ API.
 *
 * @param yaml  Null-terminated UTF-8 YAML string.
 * @return New handle (ref-count 1), or `NULL` on parse error.
 *
 * @code{.c}
 * bison_handle h = bison_from_yaml("x: 1\ny: 2.5\n");
 * bison_release(h);
 * @endcode
 */
BISON_API bison_handle bison_from_yaml(const char* yaml);

/* ═══════════════════════════════════════════════════════════════════════════
 * Export helpers
 * ═════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Serialize @p h to a JSON string.
 *
 * Field keys are emitted as `"#<decimal>"` unless a key-name map is supplied
 * at the C++ level; via this C API no map is available, so all field keys
 * appear as `"#<decimal>"`.  The returned string is heap-allocated; release
 * it with `bison_free_string`.
 *
 * @param h       Source object handle.
 * @param indent  Indentation width in spaces; pass -1 for compact output.
 * @param out     Receives a pointer to the allocated JSON string on success.
 * @return `BISON_OK`, `BISON_ERR_NULL`, or `BISON_ERR_EXCEPTION`.
 *
 * @code{.c}
 * char* json = NULL;
 * bison_to_json(h, 2, &json);
 * puts(json);
 * bison_free_string(json);
 * @endcode
 */
BISON_API bison_error bison_to_json(bison_handle h, int indent, char** out);

/**
 * @brief Serialize @p h to a YAML string.
 *
 * Produces block-style YAML.  Field keys appear as `"#<decimal>"` (same
 * limitation as `bison_to_json`).  The returned string is heap-allocated; release it with
 * `bison_free_string`.
 *
 * @param h    Source object handle.
 * @param out  Receives a pointer to the allocated YAML string on success.
 * @return `BISON_OK`, `BISON_ERR_NULL`, or `BISON_ERR_EXCEPTION`.
 *
 * @code{.c}
 * char* yaml = NULL;
 * bison_to_yaml(h, &yaml);
 * puts(yaml);
 * bison_free_string(yaml);
 * @endcode
 */
BISON_API bison_error bison_to_yaml(bison_handle h, char** out);

/* ═══════════════════════════════════════════════════════════════════════════
 * Pretty-print
 * ═════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Options controlling the output format of `bison_print`.
 *
 * Pass `NULL` to `bison_print` to use the defaults shown below.
 */
typedef struct bison_print_options {
  /** @brief Non-zero = multiline with indentation (default); 0 = single line. */
  int multiline;
  /** @brief Per-level indentation string in multiline mode.  `NULL` = `"  "`. */
  const char* indent;
} bison_print_options;

/**
 * @brief Convert @p h to a human-readable string.
 *
 * The returned string is heap-allocated; release it with `bison_free_string`.
 *
 * @param h     Source object handle.
 * @param opts  Format options, or `NULL` for defaults (multiline, 2-space indent).
 * @param out   Receives a pointer to the allocated string.
 * @return `BISON_OK`, `BISON_ERR_NULL`, or `BISON_ERR_EXCEPTION`.
 */
BISON_API bison_error bison_print(bison_handle h, const bison_print_options* opts, char** out);

/**
 * @brief Release a string returned by `bison_print`.
 *
 * @param s  Pointer returned by `bison_print`.  `NULL` is a no-op.
 */
BISON_API void bison_free_string(char* s);

/* ═══════════════════════════════════════════════════════════════════════════
 * Class registry
 * ═════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Attribute metadata for a class or a field.
 *
 * Used by `bison_add_class` and `bison_add_field_*`.
 * Any `NULL` string pointer means the corresponding attribute is not set.
 * Pass `NULL` for the whole struct to register without any attributes.
 */
typedef struct bison_attributes {
  const char* display_name; /**< Human-readable name, or `NULL`. */
  const char* description; /**< Human-readable description, or `NULL`. */
  const char* category; /**< Category group name, or `NULL`. */
  int obsolete; /**< Non-zero if deprecated. */
  const char* obsolete_message; /**< Deprecation message, or `NULL`. */
  int required; /**< Non-zero if required. */
} bison_attributes;

/**
 * @brief Register @p klass as a class prototype in a namespace.
 *
 * This registers @p klass as a class prototype in the named namespace.
 * Classes in different namespaces may share the same name without collision.
 * The `__namespace` field of @p klass is set automatically to @p ns_name.
 *
 * @param ns_name      Hash of the namespace name (use `bison_key()`); pass
 *                     `0` to register in the global (default) namespace.
 * @param klass        Handle whose `__class` field has already been set.
 *                     The library **does not** take ownership of @p klass.
 * @param parent_name  Hash of the parent class name (use `bison_key()`); pass
 *                     `0` for a root class.
 * @param meta         Optional attribute metadata; pass `NULL` for none.
 * @return `BISON_OK` on success, `BISON_ERR_DUPLICATE` if a class with the
 *         same name is already registered in @p ns_name, or `BISON_ERR_NULL`
 *         if @p klass is `NULL`.
 */
BISON_API bison_error
bison_add_class(bison_hash ns_name, bison_handle klass, bison_hash parent_name, const bison_attributes* meta);

/**
 * @brief Look up a class in a namespace.
 *
 * Performs a direct lookup in the class registry for a class in the given
 * namespace and returns the registered class prototype.
 *
 * @param ns_name    Hash of the namespace name (use `bison_key()`); pass
 *                   `0` for the global (default) namespace.
 * @param klass_name Hash of the class name to look up (use `bison_key()`).
 * @return A **non-owning** handle for the found prototype, or `NULL` if not
 *         found.  Do **not** call `bison_release` on the returned handle.
 */
BISON_API bison_handle bison_find_class(bison_hash ns_name, bison_hash klass_name);

/**
 * @brief Clear the entire class registry.
 *
 * Removes all registered classes from the registry.
 */
BISON_API void bison_clear_registry(void);

/**
 * @brief Read the class-level attributes from a registered class prototype.
 *
 * Looks up @p klass_name in @p ns_name and fills @p out with its attribute
 * metadata.  String pointers in @p out are **owned by the library** and remain
 * valid as long as the class is registered; do not free them.
 *
 * @param ns_name    Namespace hash (`0` = global).
 * @param klass_name Class name hash (use `bison_key()`).
 * @param out        Receives the class attribute metadata.
 * @return `BISON_OK`, `BISON_ERR_NOT_FOUND`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_get_class_attributes(bison_hash ns_name, bison_hash klass_name, bison_attributes* out);

/**
 * @brief Read the attributes of a named field on an object handle.
 *
 * Fills @p out with the attribute metadata for the field at @p field_key.
 * String pointers in @p out are **owned by the object** and remain valid as
 * long as @p h is alive; do not free them.
 *
 * @param h          Source object handle.
 * @param field_key  Field name hash (use `bison_key()`).
 * @param out        Receives the field attribute metadata.
 * @return `BISON_OK`, `BISON_ERR_NOT_FOUND`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_get_field_attributes(bison_handle h, bison_hash field_key, bison_attributes* out);

/**
 * @brief Read the attributes of a named method on an object handle.
 *
 * The returned strings point into the handle's storage and are valid only
 * while the handle is alive and the method's attributes are not modified.
 *
 * @param h           Source object handle.
 * @param method_key  Method name hash (use `bison_key()`).
 * @param out         Receives the method attribute metadata.
 * @return `BISON_OK`, `BISON_ERR_NOT_FOUND`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_get_method_attributes(bison_handle h, bison_hash method_key, bison_attributes* out);

/* ═══════════════════════════════════════════════════════════════════════════
 * Field access — scalar setters
 * ═════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Set an `int32_t` field by hash key.
 * @param h     Target object handle.
 * @param name  Field name hash (use `bison_key()`).
 * @param value New value.
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error bison_set_int(bison_handle h, bison_hash name, int32_t value);

/**
 * @brief Set a `float` field by hash key.
 * @param h     Target object handle.
 * @param name  Field name hash (use `bison_key()`).
 * @param value New value.
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error bison_set_float(bison_handle h, bison_hash name, float value);

/**
 * @brief Set a `bool` field by hash key.
 * @param h     Target object handle.
 * @param name  Field name hash (use `bison_key()`).
 * @param value New value (non-zero = true).
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error bison_set_bool(bison_handle h, bison_hash name, int value);

/**
 * @brief Set a `std::string` field by hash key.
 * @param h     Target object handle.
 * @param name  Field name hash (use `bison_key()`).
 * @param value Null-terminated string value (copied internally).
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error bison_set_string(bison_handle h, bison_hash name, const char* value);

/**
 * @brief Set a `bdg::bison::key_t` (hashed-name) field by hash key.
 *
 * `key_t` is its own `field` alternative, distinct from a plain `int32_t`
 * field -- use this (not `bison_set_int`) for any field the C++ side reads
 * with `dynamic::as<bdg::bison::key_t>()`, e.g. an object's `"id"` field or
 * an enum-like field such as a `"nav_mode"` selector.
 *
 * @param h     Target object handle.
 * @param name  Field name hash (use `bison_key()`).
 * @param value New value -- a pre-hashed key (use `bison_key()`).
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error bison_set_key(bison_handle h, bison_hash name, bison_hash value);

/**
 * @brief Set a nested `dynamic` object field by hash key.
 *
 * The library **increments** the ref-count of @p value so both the caller and
 * the owning object share the same underlying instance.
 *
 * @param h     Target object handle.
 * @param name  Field name hash (use `bison_key()`).
 * @param value Handle to set as the field value (may be `NULL` for a null ref).
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error bison_set_object(bison_handle h, bison_hash name, bison_handle value);

/**
 * @brief Set an `int32_t` field by numeric (array) index.
 * @param h     Target object handle.
 * @param index Zero-based numeric index.
 * @param value New value.
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error bison_set_int_at(bison_handle h, size_t index, int32_t value);

/**
 * @brief Set a `float` field by numeric index.
 * @param h     Target object handle.
 * @param index Zero-based numeric index.
 * @param value New value.
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error bison_set_float_at(bison_handle h, size_t index, float value);

/**
 * @brief Set a string field by numeric index.
 * @param h     Target object handle.
 * @param index Zero-based numeric index.
 * @param value Null-terminated string (copied internally).
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error bison_set_string_at(bison_handle h, size_t index, const char* value);

/**
 * @brief Set a `bool` field by numeric index.
 * @param h     Target object handle.
 * @param index Zero-based numeric index.
 * @param value New value (non-zero = true).
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error bison_set_bool_at(bison_handle h, size_t index, int value);

/**
 * @brief Set a `bdg::bison::key_t` (hashed-name) field by numeric index.
 *
 * See `bison_set_key()` for why this is a distinct alternative from
 * `bison_set_int_at()`.
 *
 * @param h     Target object handle.
 * @param index Zero-based numeric index.
 * @param value New value -- a pre-hashed key (use `bison_key()`).
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error bison_set_key_at(bison_handle h, size_t index, bison_hash value);

/**
 * @brief Set a nested `dynamic` object field by numeric index.
 *
 * The library **increments** the ref-count of @p value so both the caller and
 * the owning object share the same underlying instance.
 *
 * @param h     Target object handle.
 * @param index Zero-based numeric index.
 * @param value Handle to set as the field value (may be `NULL` for a null ref).
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error bison_set_object_at(bison_handle h, size_t index, bison_handle value);

/* ═══════════════════════════════════════════════════════════════════════════
 * Field access — scalar getters
 * ═════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Read an `int32_t` field by hash key.
 * @param h     Source object handle.
 * @param name  Field name hash (use `bison_key()`).
 * @param[out] out  Receives the value on success.
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_get_int(bison_handle h, bison_hash name, int32_t* out);

/**
 * @brief Read a `float` field by hash key.
 * @param h     Source object handle.
 * @param name  Field name hash (use `bison_key()`).
 * @param[out] out  Receives the value on success.
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_get_float(bison_handle h, bison_hash name, float* out);

/**
 * @brief Read a `bool` field by hash key.
 * @param h     Source object handle.
 * @param name  Field name hash (use `bison_key()`).
 * @param[out] out  Receives 1 (true) or 0 (false) on success.
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_get_bool(bison_handle h, bison_hash name, int* out);

/**
 * @brief Read a string field by hash key.
 *
 * If @p buf is non-null the string is copied into @p buf (null-terminated)
 * up to @p buf_len bytes.  If @p buf is `NULL` only the required length is
 * returned in @p len_out.  @p len_out may be `NULL` if the caller does not
 * need the length.
 *
 * @param h        Source object handle.
 * @param name     Field name hash (use `bison_key()`).
 * @param buf      Output buffer (may be `NULL` to query length).
 * @param buf_len  Size of @p buf in bytes (including space for the null
 * terminator).
 * @param[out] len_out  Set to the string length (excluding null terminator).
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_get_string(bison_handle h, bison_hash name, char* buf, size_t buf_len, size_t* len_out);

/**
 * @brief Read a `bdg::bison::key_t` (hashed-name) field by hash key.
 *
 * @param h     Source object handle.
 * @param name  Field name hash (use `bison_key()`).
 * @param[out] out  Receives the value on success, as its raw hash.
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_get_key(bison_handle h, bison_hash name, bison_hash* out);

/**
 * @brief Read a nested object field by hash key.
 *
 * Returns a **new** handle (ref-count 1) that must be released by the caller.
 *
 * @param h     Source object handle.
 * @param name  Field name hash (use `bison_key()`).
 * @param[out] out  Set to the child handle on success (may be `NULL` for null
 *                  dynamic refs).
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_get_object(bison_handle h, bison_hash name, bison_handle* out);

/**
 * @brief Read an `int32_t` field by numeric index.
 * @param h     Source object handle.
 * @param index Zero-based numeric index.
 * @param[out] out  Receives the value on success.
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_get_int_at(bison_handle h, size_t index, int32_t* out);

/**
 * @brief Read a `float` field by numeric index.
 * @param h     Source object handle.
 * @param index Zero-based numeric index.
 * @param[out] out  Receives the value on success.
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_get_float_at(bison_handle h, size_t index, float* out);

/**
 * @brief Read a string field by numeric index.
 *
 * Same copy semantics as `bison_get_string`.
 *
 * @param h        Source object handle.
 * @param index    Zero-based numeric index.
 * @param buf      Output buffer (may be `NULL` to query length).
 * @param buf_len  Size of @p buf in bytes.
 * @param[out] len_out  Set to the string length.
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_get_string_at(bison_handle h, size_t index, char* buf, size_t buf_len, size_t* len_out);

/**
 * @brief Read a `bool` field by numeric index.
 * @param h     Source object handle.
 * @param index Zero-based numeric index.
 * @param[out] out  Receives 1 (true) or 0 (false) on success.
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_get_bool_at(bison_handle h, size_t index, int* out);

/**
 * @brief Read a `bdg::bison::key_t` (hashed-name) field by numeric index.
 * @param h     Source object handle.
 * @param index Zero-based numeric index.
 * @param[out] out  Receives the value on success, as its raw hash.
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_get_key_at(bison_handle h, size_t index, bison_hash* out);

/**
 * @brief Read a nested object field by numeric index.
 *
 * Returns a **new** handle (ref-count 1) that must be released by the caller.
 *
 * @param h     Source object handle.
 * @param index Zero-based numeric index.
 * @param[out] out  Set to the child handle on success (may be `NULL` for null
 *                  dynamic refs).
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_get_object_at(bison_handle h, size_t index, bison_handle* out);

/**
 * @brief Return the number of array-like (numeric-key) elements.
 * @param h  Source object handle.
 * @return Element count, or 0 if @p h is `NULL`.
 */
BISON_API size_t bison_size(bison_handle h);

/* ═══════════════════════════════════════════════════════════════════════════
 * Methods
 * ═════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Register a C callback as a named method on @p h.
 *
 * The callback @p fn is invoked whenever the method is called.
 *
 * @param h        Target object handle.
 * @param name     Method name hash (use `bison_key()`).
 * @param fn       Function pointer implementing the method.
 * @param user     Arbitrary user context (may be `NULL`).
 * @param meta     Optional attribute annotations; pass `NULL` for none.
 * @return `BISON_OK`, `BISON_ERR_DUPLICATE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error
bison_add_method(bison_handle h, bison_hash name, bison_method_fn fn, void* user, const bison_attributes* meta);

/**
 * @brief Invoke a named method on @p h.
 *
 * @param h       Target object handle (becomes `self` inside the callback).
 * @param name    Method name hash (use `bison_key()`).
 * @param params  Handle containing call arguments (may be an empty object).
 * @param[out] result  Set to a **new** handle (ref-count 1) holding the return
 *                     value.  Caller must release it.
 * @return `BISON_OK`, `BISON_ERR_NOT_FOUND`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_call(bison_handle h, bison_hash name, bison_handle params, bison_handle* result);

/* ═══════════════════════════════════════════════════════════════════════════
 * Field registration with optional attribute metadata
 * ═════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Add an `int32_t` field to @p obj with optional attribute metadata.
 *
 * @param obj   Target object handle.
 * @param key   Field name hash (use `bison_key()`).
 * @param value Initial integer value.
 * @param meta  Optional attribute metadata; pass `NULL` for none.
 * @return `BISON_OK`, `BISON_ERR_NULL`, `BISON_ERR_DUPLICATE`, or
 *         `BISON_ERR_EXCEPTION`.
 */
BISON_API bison_error
bison_add_field_int(bison_handle obj, bison_hash key, int32_t value, const bison_attributes* meta);

/**
 * @brief Add a `float` field to @p obj with optional attribute metadata.
 *
 * @param obj   Target object handle.
 * @param key   Field name hash (use `bison_key()`).
 * @param value Initial float value.
 * @param meta  Optional attribute metadata; pass `NULL` for none.
 * @return `BISON_OK`, `BISON_ERR_NULL`, `BISON_ERR_DUPLICATE`, or
 *         `BISON_ERR_EXCEPTION`.
 */
BISON_API bison_error
bison_add_field_float(bison_handle obj, bison_hash key, float value, const bison_attributes* meta);

/**
 * @brief Add a `bool` field to @p obj with optional attribute metadata.
 *
 * @param obj   Target object handle.
 * @param key   Field name hash (use `bison_key()`).
 * @param value Non-zero for `true`, zero for `false`.
 * @param meta  Optional attribute metadata; pass `NULL` for none.
 * @return `BISON_OK`, `BISON_ERR_NULL`, `BISON_ERR_DUPLICATE`, or
 *         `BISON_ERR_EXCEPTION`.
 */
BISON_API bison_error bison_add_field_bool(bison_handle obj, bison_hash key, int value, const bison_attributes* meta);

/**
 * @brief Add a `string` field to @p obj with optional attribute metadata.
 *
 * @param obj   Target object handle.
 * @param key   Field name hash (use `bison_key()`).
 * @param value Null-terminated initial string value.
 * @param meta  Optional attribute metadata; pass `NULL` for none.
 * @return `BISON_OK`, `BISON_ERR_NULL`, `BISON_ERR_DUPLICATE`, or
 *         `BISON_ERR_EXCEPTION`.
 */
BISON_API bison_error
bison_add_field_string(bison_handle obj, bison_hash key, const char* value, const bison_attributes* meta);

/**
 * @brief Add a `bdg::bison::key_t` field to @p obj with optional attribute
 * metadata.
 *
 * @param obj   Target object handle.
 * @param key   Field name hash (use `bison_key()`).
 * @param value Initial value -- a pre-hashed key (use `bison_key()`).
 * @param meta  Optional attribute metadata; pass `NULL` for none.
 * @return `BISON_OK`, `BISON_ERR_NULL`, `BISON_ERR_DUPLICATE`, or
 *         `BISON_ERR_EXCEPTION`.
 */
BISON_API bison_error
bison_add_field_key(bison_handle obj, bison_hash key, bison_hash value, const bison_attributes* meta);

/**
 * @brief Add a `vector<bool>` field to @p obj with optional attribute metadata.
 *
 * @param obj    Target object handle.
 * @param key    Field name hash (use `bison_key()`).
 * @param values Array of initial values (non-zero is `true`); may be `NULL`
 *               when @p count is 0, producing an empty vector.
 * @param count  Number of elements in @p values.
 * @param meta   Optional attribute metadata; pass `NULL` for none.
 * @return `BISON_OK`, `BISON_ERR_NULL`, `BISON_ERR_DUPLICATE`, or
 *         `BISON_ERR_EXCEPTION`.
 */
BISON_API bison_error bison_add_field_vector_bool(
    bison_handle obj,
    bison_hash key,
    const int* values,
    size_t count,
    const bison_attributes* meta);

/**
 * @brief Add a `vector<int32_t>` field to @p obj with optional attribute
 * metadata.
 *
 * @param obj    Target object handle.
 * @param key    Field name hash (use `bison_key()`).
 * @param values Array of initial values; may be `NULL` when @p count is 0,
 *               producing an empty vector.
 * @param count  Number of elements in @p values.
 * @param meta   Optional attribute metadata; pass `NULL` for none.
 * @return `BISON_OK`, `BISON_ERR_NULL`, `BISON_ERR_DUPLICATE`, or
 *         `BISON_ERR_EXCEPTION`.
 */
BISON_API bison_error bison_add_field_vector_int(
    bison_handle obj,
    bison_hash key,
    const int32_t* values,
    size_t count,
    const bison_attributes* meta);

/**
 * @brief Add a `vector<float>` field to @p obj with optional attribute
 * metadata.
 *
 * @param obj    Target object handle.
 * @param key    Field name hash (use `bison_key()`).
 * @param values Array of initial values; may be `NULL` when @p count is 0,
 *               producing an empty vector.
 * @param count  Number of elements in @p values.
 * @param meta   Optional attribute metadata; pass `NULL` for none.
 * @return `BISON_OK`, `BISON_ERR_NULL`, `BISON_ERR_DUPLICATE`, or
 *         `BISON_ERR_EXCEPTION`.
 */
BISON_API bison_error bison_add_field_vector_float(
    bison_handle obj,
    bison_hash key,
    const float* values,
    size_t count,
    const bison_attributes* meta);

/**
 * @brief Add a `vector<uint8_t>` (raw byte buffer) field to @p obj with
 * optional attribute metadata.
 *
 * @param obj    Target object handle.
 * @param key    Field name hash (use `bison_key()`).
 * @param values Array of initial bytes; may be `NULL` when @p count is 0,
 *               producing an empty vector.
 * @param count  Number of bytes in @p values.
 * @param meta   Optional attribute metadata; pass `NULL` for none.
 * @return `BISON_OK`, `BISON_ERR_NULL`, `BISON_ERR_DUPLICATE`, or
 *         `BISON_ERR_EXCEPTION`.
 */
BISON_API bison_error bison_add_field_vector_bytes(
    bison_handle obj,
    bison_hash key,
    const uint8_t* values,
    size_t count,
    const bison_attributes* meta);

/* ═══════════════════════════════════════════════════════════════════════════
 * Vector field access
 *
 * `bison_add_field_vector_*()` above declares a vector-typed field once, at
 * registration time.  These functions read and replace its value
 * afterward -- the vector-field counterpart of the scalar getters/setters
 * above.  As with `bison_get_string()`, a vector getter follows a two-call
 * convention: call once with @p buf `NULL` to learn the required element
 * count via @p len_out, then again with a caller-sized buffer.
 * ═════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Read a `vector<bool>` field.
 *
 * Each element is written to @p buf as `0` or `1`.  If @p buf is `NULL` only
 * the required element count is returned in @p len_out.
 *
 * @param h        Source object handle.
 * @param name     Field name hash (use `bison_key()`).
 * @param buf      Output buffer (may be `NULL` to query length); `int` rather
 *                 than `bool` since C has no fixed-width boolean type.
 * @param buf_len  Capacity of @p buf, in elements.
 * @param[out] len_out  Set to the element count (may be `NULL`).
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_get_vector_bool(bison_handle h, bison_hash name, int* buf, size_t buf_len, size_t* len_out);

/**
 * @brief Read a `vector<int32_t>` field. Same two-call convention as
 *        `bison_get_vector_bool()`.
 * @param h        Source object handle.
 * @param name     Field name hash (use `bison_key()`).
 * @param buf      Output buffer (may be `NULL` to query length).
 * @param buf_len  Capacity of @p buf, in elements.
 * @param[out] len_out  Set to the element count (may be `NULL`).
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error
bison_get_vector_int(bison_handle h, bison_hash name, int32_t* buf, size_t buf_len, size_t* len_out);

/**
 * @brief Read a `vector<float>` field. Same two-call convention as
 *        `bison_get_vector_bool()`.
 * @param h        Source object handle.
 * @param name     Field name hash (use `bison_key()`).
 * @param buf      Output buffer (may be `NULL` to query length).
 * @param buf_len  Capacity of @p buf, in elements.
 * @param[out] len_out  Set to the element count (may be `NULL`).
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error
bison_get_vector_float(bison_handle h, bison_hash name, float* buf, size_t buf_len, size_t* len_out);

/**
 * @brief Read a `vector<uint8_t>` (raw byte buffer) field. Same two-call
 *        convention as `bison_get_vector_bool()`.
 * @param h        Source object handle.
 * @param name     Field name hash (use `bison_key()`).
 * @param buf      Output buffer (may be `NULL` to query length).
 * @param buf_len  Capacity of @p buf, in bytes.
 * @param[out] len_out  Set to the byte count (may be `NULL`).
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error
bison_get_vector_bytes(bison_handle h, bison_hash name, uint8_t* buf, size_t buf_len, size_t* len_out);

/**
 * @brief Replace the contents of a `vector<bool>` field, auto-vivifying it
 *        if absent.
 *
 * @param h      Target object handle.
 * @param name   Field name hash (use `bison_key()`).
 * @param values Array of new values (non-zero is `true`); may be `NULL`
 *               when @p count is 0, producing an empty vector.
 * @param count  Number of elements in @p values.
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_set_vector_bool(bison_handle h, bison_hash name, const int* values, size_t count);

/**
 * @brief Replace the contents of a `vector<int32_t>` field, auto-vivifying
 *        it if absent.
 * @param h      Target object handle.
 * @param name   Field name hash (use `bison_key()`).
 * @param values Array of new values; may be `NULL` when @p count is 0.
 * @param count  Number of elements in @p values.
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_set_vector_int(bison_handle h, bison_hash name, const int32_t* values, size_t count);

/**
 * @brief Replace the contents of a `vector<float>` field, auto-vivifying it
 *        if absent.
 * @param h      Target object handle.
 * @param name   Field name hash (use `bison_key()`).
 * @param values Array of new values; may be `NULL` when @p count is 0.
 * @param count  Number of elements in @p values.
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_set_vector_float(bison_handle h, bison_hash name, const float* values, size_t count);

/**
 * @brief Replace the contents of a `vector<uint8_t>` (raw byte buffer)
 *        field, auto-vivifying it if absent.
 * @param h      Target object handle.
 * @param name   Field name hash (use `bison_key()`).
 * @param values Array of new bytes; may be `NULL` when @p count is 0.
 * @param count  Number of bytes in @p values.
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_set_vector_bytes(bison_handle h, bison_hash name, const uint8_t* values, size_t count);

/* ═══════════════════════════════════════════════════════════════════════════
 * Binary serialization
 *
 * Raw, self-describing binary encoding of a `dynamic` object -- the ABI
 * counterpart of the internal `dynamic::serialize(buffer_serializer&)` /
 * `dynamic::deserialize(buffer_deserializer&)`.  Field keys are encoded as
 * their `bison_hash` values (see `FORMAT.md`), not names, so this format is
 * self-contained and requires no key-name map to round-trip -- unlike
 * `bison_to_json()`/`bison_to_yaml()`, which are for human-readable/
 * interop use, this is the compact wire format used by `dynamic::serialize`.
 * ═════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Serialize @p h to a binary buffer (the `dynamic::serialize()` wire
 *        format; see `FORMAT.md`).
 *
 * The returned buffer is heap-allocated; release it with
 * `bison_free_buffer()`.
 *
 * @param h         Source object handle.
 * @param out       Receives a pointer to the allocated buffer on success.
 * @param out_len   Receives the buffer length in bytes on success.
 * @return `BISON_OK`, `BISON_ERR_NULL`, or `BISON_ERR_EXCEPTION`.
 *
 * @code{.c}
 * uint8_t* buf = NULL;
 * size_t len = 0;
 * bison_serialize(h, &buf, &len);
 * // ... send buf/len over a socket, write to a file, etc ...
 * bison_free_buffer(buf);
 * @endcode
 */
BISON_API bison_error bison_serialize(bison_handle h, uint8_t** out, size_t* out_len);

/**
 * @brief Deserialize a binary buffer produced by `bison_serialize()`.
 *
 * @param data  Buffer to decode.
 * @param len   Length of @p data in bytes.
 * @param[out] out  Receives the decoded handle (ref-count 1) on success.
 * @return `BISON_OK`, `BISON_ERR_NULL`, or `BISON_ERR_PARSE` on malformed
 *         input (including buffer underflow).
 */
BISON_API bison_error bison_deserialize(const uint8_t* data, size_t len, bison_handle* out);

/**
 * @brief Release a buffer returned by `bison_serialize()`.
 * @param buf  Pointer returned by `bison_serialize()`.  `NULL` is a no-op.
 */
BISON_API void bison_free_buffer(uint8_t* buf);

/* ═══════════════════════════════════════════════════════════════════════════
 * Utility
 * ═════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Compute the FNV-1a hash of a null-terminated string.
 *
 * This is the same hash function used internally by the C++ `"name"_key`
 * literal and `bdg::bison::hash()`.  Use it to prepare keys for functions that
 * accept pre-hashed `bison_hash` names (e.g. `bison_create`,
 * `bison_add_class`).
 *
 * @param name  Null-terminated string.
 * @return 32-bit FNV-1a hash with the high bit set.
 */
BISON_API bison_hash bison_key(const char* name);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* BISON_C_H */
