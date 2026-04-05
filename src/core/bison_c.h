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

/* ─── Opaque handle ──────────────────────────────────────────────────────── */

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
  BISON_ERR_DUPLICATE =
      -4, /**< Attempted to add a duplicate class or method. */
  BISON_ERR_EXCEPTION = -5, /**< An unexpected C++ exception was caught. */
  BISON_ERR_PARSE = -6, /**< Input string failed to parse (JSON / YAML). */
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
typedef void (*bison_method_fn)(
    bison_handle self,
    bison_handle params,
    bison_handle result,
    void* user);

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
 * `dynamic::instantiate`.
 *
 * This is equivalent to `bison_create(klass_name)` but mirrors the C++ API
 * naming and behavior explicitly.  The object is placed in the global (default)
 * namespace; use `bison_instantiate_ns()` to target a named namespace.
 *
 * @param klass_name  Hashed class name (use `bison_key()` to compute).
 * @return New handle (ref-count 1) or `NULL` on allocation failure.
 */
BISON_API bison_handle bison_instantiate(bison_hash klass_name);

/**
 * @brief Create a new dynamic object in a specific namespace.
 *
 * Sets the `__namespace` field on the new instance so that subsequent field,
 * method, and class lookups target the correct namespace collection.
 *
 * @param ns_name     Hash of the namespace name (use `bison_key()`); pass
 *                    `0` for the global (default) namespace.
 * @param klass_name  Hashed class name (use `bison_key()` to compute).
 * @return New handle (ref-count 1) or `NULL` on allocation failure.
 */
BISON_API bison_handle
bison_instantiate_ns(bison_hash ns_name, bison_hash klass_name);

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
 * Class registry
 * ═════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Register @p klass as a class prototype in the global (default)
 *        namespace of the class registry.
 *
 * This is equivalent to calling `bison_add_class_ns(0, klass, parent_name)`.
 * All classes registered with this function share the global namespace and
 * are visible to objects whose `__namespace` field is `0` (or absent).
 *
 * @param parent_name  Hash of the parent class name (use `bison_key()`); pass
 *                     `0` for a root class.
 * @param klass        Handle whose `__class` field has already been set.
 *                     The library **does not** take ownership of @p klass.
 * @return `BISON_OK` on success, `BISON_ERR_DUPLICATE` if a class with the
 *         same name is already registered in the global namespace, or
 *         `BISON_ERR_NULL` if @p klass is `NULL`.
 */
BISON_API bison_error
bison_add_class(bison_hash parent_name, bison_handle klass);

/**
 * @brief Register @p klass as a class prototype in a named namespace.
 *
 * Classes in different namespaces may share the same name without collision.
 * The `__namespace` field of @p klass is set automatically to @p ns_name.
 * Use `bison_instantiate_ns()` (or set `__namespace` on the instance before
 * any field lookup) to direct lookups to the correct namespace.
 *
 * @param ns_name      Hash of the namespace name (use `bison_key()`); pass
 *                     `0` to register in the global (default) namespace.
 * @param klass        Handle whose `__class` field has already been set.
 *                     The library **does not** take ownership of @p klass.
 * @param parent_name  Hash of the parent class name (use `bison_key()`); pass
 *                     `0` for a root class.
 * @return `BISON_OK` on success, `BISON_ERR_DUPLICATE` if a class with the
 *         same name is already registered in @p ns_name, or `BISON_ERR_NULL`
 *         if @p klass is `NULL`.
 */
BISON_API bison_error bison_add_class_ns(
    bison_hash ns_name,
    bison_handle klass,
    bison_hash parent_name);

/**
 * @brief Search the class hierarchy of @p h for a registered class.
 *
 * Starts from the class of @p h and walks up parent links until @p name is
 * found or the chain ends.
 *
 * @param h     Handle to search from.
 * @param name  Hash of the class name to look for (use `bison_key()`).
 * @return A **non-owning** handle for the found prototype, or `NULL` if not
 *         found.  Do **not** call `bison_release` on the returned handle.
 */
BISON_API bison_handle bison_find_class(bison_handle h, bison_hash name);

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
BISON_API bison_error
bison_set_int(bison_handle h, bison_hash name, int32_t value);

/**
 * @brief Set a `float` field by hash key.
 * @param h     Target object handle.
 * @param name  Field name hash (use `bison_key()`).
 * @param value New value.
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error
bison_set_float(bison_handle h, bison_hash name, float value);

/**
 * @brief Set a `bool` field by hash key.
 * @param h     Target object handle.
 * @param name  Field name hash (use `bison_key()`).
 * @param value New value (non-zero = true).
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error
bison_set_bool(bison_handle h, bison_hash name, int value);

/**
 * @brief Set a `std::string` field by hash key.
 * @param h     Target object handle.
 * @param name  Field name hash (use `bison_key()`).
 * @param value Null-terminated string value (copied internally).
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error
bison_set_string(bison_handle h, bison_hash name, const char* value);

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
BISON_API bison_error
bison_set_object(bison_handle h, bison_hash name, bison_handle value);

/**
 * @brief Set an `int32_t` field by numeric (array) index.
 * @param h     Target object handle.
 * @param index Zero-based numeric index.
 * @param value New value.
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error
bison_set_int_at(bison_handle h, size_t index, int32_t value);

/**
 * @brief Set a `float` field by numeric index.
 * @param h     Target object handle.
 * @param index Zero-based numeric index.
 * @param value New value.
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error
bison_set_float_at(bison_handle h, size_t index, float value);

/**
 * @brief Set a string field by numeric index.
 * @param h     Target object handle.
 * @param index Zero-based numeric index.
 * @param value Null-terminated string (copied internally).
 * @return `BISON_OK` or an error code.
 */
BISON_API bison_error
bison_set_string_at(bison_handle h, size_t index, const char* value);

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
BISON_API bison_error
bison_get_int(bison_handle h, bison_hash name, int32_t* out);

/**
 * @brief Read a `float` field by hash key.
 * @param h     Source object handle.
 * @param name  Field name hash (use `bison_key()`).
 * @param[out] out  Receives the value on success.
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error
bison_get_float(bison_handle h, bison_hash name, float* out);

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
BISON_API bison_error bison_get_string(
    bison_handle h,
    bison_hash name,
    char* buf,
    size_t buf_len,
    size_t* len_out);

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
BISON_API bison_error
bison_get_object(bison_handle h, bison_hash name, bison_handle* out);

/**
 * @brief Read an `int32_t` field by numeric index.
 * @param h     Source object handle.
 * @param index Zero-based numeric index.
 * @param[out] out  Receives the value on success.
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error
bison_get_int_at(bison_handle h, size_t index, int32_t* out);

/**
 * @brief Read a `float` field by numeric index.
 * @param h     Source object handle.
 * @param index Zero-based numeric index.
 * @param[out] out  Receives the value on success.
 * @return `BISON_OK`, `BISON_ERR_TYPE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error
bison_get_float_at(bison_handle h, size_t index, float* out);

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
BISON_API bison_error bison_get_string_at(
    bison_handle h,
    size_t index,
    char* buf,
    size_t buf_len,
    size_t* len_out);

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
 * The callback @p fn is wrapped in a lambda and stored as a
 * `bdg::bison::method`. The @p user pointer is passed through to every
 * invocation without any lifetime management; callers must ensure it remains
 * valid for as long as the method is reachable.
 *
 * @param h     Target object handle.
 * @param name  Method name hash (use `bison_key()`).
 * @param fn    Function pointer implementing the method.
 * @param user  Arbitrary user context (may be `NULL`).
 * @return `BISON_OK`, `BISON_ERR_DUPLICATE`, or `BISON_ERR_NULL`.
 */
BISON_API bison_error bison_add_method(
    bison_handle h,
    bison_hash name,
    bison_method_fn fn,
    void* user);

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
BISON_API bison_error bison_call(
    bison_handle h,
    bison_hash name,
    bison_handle params,
    bison_handle* result);

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

/* ═══════════════════════════════════════════════════════════════════════════
 * RMI – Remote Method Invocation (plain-C interface)
 *
 * The RMI C API exposes the in-memory transport, server, client, and remote
 * object proxy as opaque handles.  The typical lifecycle is:
 *
 *   1. Create a transport:   bison_rmi_transport t =
 * bison_rmi_transport_create();
 *   2. Create a server:      bison_rmi_server  srv =
 * bison_rmi_server_create(t);
 *   3. Start the server:     bison_rmi_server_listen(srv);
 *   4. Create clients:       bison_rmi_client  c   =
 * bison_rmi_client_create(t);
 *   5. Connect clients:      bison_rmi_client_connect(c);
 *   6. Instantiate objects:  bison_rmi_proxy proxy =
 * bison_rmi_client_instantiate(c, klass, NULL);
 *   7. Use objects:          bison_rmi_proxy_call(proxy, params, &result);
 *   8. Destroy proxy:        bison_rmi_client_destroy_proxy(c, proxy);
 *   9. Disconnect client:    bison_rmi_client_disconnect(c);
 *  10. Free client:          bison_rmi_client_destroy(c);
 *  11. Stop server:          bison_rmi_server_stop(srv);
 *  12. Free server:          bison_rmi_server_destroy(srv);
 *  13. Free transport:       bison_rmi_transport_destroy(t);
 *
 * Ownership: the transport must outlive both the server and all clients.
 * Servers and clients must be stopped/disconnected before being freed.
 * ═════════════════════════════════════════════════════════════════════════ */

/** @brief Opaque handle for an in-memory RMI transport. */
typedef struct bison_rmi_transport_* bison_rmi_transport;

/** @brief Opaque handle for an RMI server. */
typedef struct bison_rmi_server_* bison_rmi_server;

/** @brief Opaque handle for an RMI client. */
typedef struct bison_rmi_client_* bison_rmi_client;

/** @brief Opaque handle for a remote object proxy. */
typedef struct bison_rmi_proxy_* bison_rmi_proxy;

/**
 * @brief Callback invoked when a server-initiated event is received.
 *
 * @param params  A read-only bison_handle containing the event parameters.
 *                The handle is valid only for the duration of the callback;
 *                callers must NOT release it.
 * @param user    User-defined context pointer passed to
 * bison_rmi_proxy_on_event.
 */
typedef void (*bison_rmi_event_fn)(bison_handle params, void* user);

/* ── Transport ──────────────────────────────────────────────────────────────
 */

/**
 * @brief Create a new in-memory server transport.
 *
 * The transport must remain alive for the lifetime of any server or client
 * that was created from it.
 *
 * @return New transport handle, or NULL on allocation failure.
 */
BISON_API bison_rmi_transport bison_rmi_transport_create(void);

/**
 * @brief Free an in-memory transport.
 *
 * The transport must not be used after this call.  All servers and clients
 * derived from it must already have been stopped/disconnected and destroyed.
 *
 * @param t  Transport handle to free.  NULL is silently ignored.
 */
BISON_API void bison_rmi_transport_destroy(bison_rmi_transport t);

/* ── Server ─────────────────────────────────────────────────────────────────
 */

/**
 * @brief Create an RMI server backed by the given in-memory transport.
 *
 * The server does not start listening until bison_rmi_server_listen() is
 * called.  The @p t handle must remain valid until after
 * bison_rmi_server_stop() and bison_rmi_server_destroy() have been called.
 *
 * @param t  Transport handle (borrowed; must outlive the server).
 * @return   New server handle, or NULL on failure.
 */
BISON_API bison_rmi_server bison_rmi_server_create(bison_rmi_transport t);

/**
 * @brief Start the server's accept loop and begin accepting clients.
 *
 * @param srv  Server handle.
 * @return BISON_OK on success, BISON_ERR_NULL if srv is NULL, or
 *         BISON_ERR_EXCEPTION if an internal error occurred.
 */
BISON_API bison_error bison_rmi_server_listen(bison_rmi_server srv);

/**
 * @brief Stop the server, close all connections, and join worker threads.
 *
 * @param srv  Server handle.
 */
BISON_API void bison_rmi_server_stop(bison_rmi_server srv);

/**
 * @brief Free the server object.
 *
 * The server must have been stopped before calling this function.
 *
 * @param srv  Server handle to free.  NULL is silently ignored.
 */
BISON_API void bison_rmi_server_destroy(bison_rmi_server srv);

/* ── Client ─────────────────────────────────────────────────────────────────
 */

/**
 * @brief Create a new RMI client connected to the given in-memory transport.
 *
 * This internally calls transport.connect() to obtain the client-side channel.
 * The client is not yet connected to the server; call
 * bison_rmi_client_connect().
 *
 * @param t  Transport handle (borrowed; must outlive the client).
 * @return   New client handle, or NULL on failure.
 */
BISON_API bison_rmi_client bison_rmi_client_create(bison_rmi_transport t);

/**
 * @brief Connect the client to the server and start the worker thread.
 *
 * Must be called before any other client operations.
 *
 * @param c  Client handle.
 * @return BISON_OK on success or a negative error code on failure.
 */
BISON_API bison_error bison_rmi_client_connect(bison_rmi_client c);

/**
 * @brief Gracefully disconnect the client and join the worker thread.
 *
 * @param c  Client handle.
 * @return BISON_OK on success or a negative error code on failure.
 */
BISON_API bison_error bison_rmi_client_disconnect(bison_rmi_client c);

/**
 * @brief Query the server for registered class metadata.
 *
 * @param c      Client handle.
 * @param klass  Hashed class name to query, or 0 to list all classes.
 * @return A new bison_handle (caller must release) containing the descriptor,
 *         or NULL on failure.
 */
BISON_API bison_handle
bison_rmi_client_describe(bison_rmi_client c, bison_hash klass);

/**
 * @brief Instantiate a remote object of the given class on the server.
 *
 * @param c       Client handle.
 * @param klass   Hashed class name (must be registered on the server).
 * @param params  Optional construction parameters (may be NULL).
 * @return A new proxy handle, or NULL on failure.
 */
BISON_API bison_rmi_proxy bison_rmi_client_instantiate(
    bison_rmi_client c,
    bison_hash klass,
    bison_handle params);

/**
 * @brief Destroy a remote object and release its owning proxy.
 *
 * The proxy handle is invalidated by this call and must not be used again.
 *
 * @param c      Client handle.
 * @param proxy  Proxy handle to destroy.
 * @return BISON_OK on success or a negative error code on failure.
 */
BISON_API bison_error
bison_rmi_client_destroy_proxy(bison_rmi_client c, bison_rmi_proxy proxy);

/**
 * @brief Free the client object.
 *
 * The client should be disconnected before calling this function.
 *
 * @param c  Client handle to free.  NULL is silently ignored.
 */
BISON_API void bison_rmi_client_destroy(bison_rmi_client c);

/* ── Remote proxy operations ────────────────────────────────────────────────
 */

/**
 * @brief Clear all explicitly set fields on the remote object.
 *
 * @param proxy  Proxy handle.
 * @return BISON_OK on success or a negative error code on failure.
 */
BISON_API bison_error bison_rmi_proxy_clear(bison_rmi_proxy proxy);

/**
 * @brief Apply a partial field update to the remote object.
 *
 * Fields present in @p fields are applied; fields not present are unchanged.
 *
 * @param proxy   Proxy handle.
 * @param fields  bison_handle containing the fields to set.
 * @return BISON_OK on success or a negative error code on failure.
 */
BISON_API bison_error
bison_rmi_proxy_set(bison_rmi_proxy proxy, bison_handle fields);

/**
 * @brief Retrieve fields from the remote object.
 *
 * If @p fields_out points to NULL or an empty dynamic, a full snapshot is
 * returned.  If @p fields_out points to a projection shape, only those fields
 * are filled (GraphQL-style).
 *
 * @param proxy      Proxy handle.
 * @param fields_out On entry: pointer to a bison_handle containing the
 *                   projection (or NULL / empty for full snapshot).
 *                   On success: updated to point to a new bison_handle
 *                   owning the result (caller must release).
 * @return BISON_OK on success or a negative error code on failure.
 */
BISON_API bison_error
bison_rmi_proxy_get(bison_rmi_proxy proxy, bison_handle* fields_out);

/**
 * @brief Invoke a method on the remote object (blocking).
 *
 * @param proxy        Proxy handle.
 * @param method_name  Hashed method name token.
 * @param params       bison_handle containing call arguments.
 * @param result_out   Receives a new bison_handle with the call result (caller
 *                    must release).  May be NULL if the result is not needed.
 * @return BISON_OK on success or a negative error code on failure.
 */
BISON_API bison_error bison_rmi_proxy_call(
    bison_rmi_proxy proxy,
    bison_hash method_name,
    bison_handle params,
    bison_handle* result_out);

/**
 * @brief Invoke a method on the remote object without waiting for a response.
 *
 * @param proxy   Proxy handle.
 * @param method_name  Hashed method name token.
 * @param params      bison_handle containing call arguments.
 * @return BISON_OK on success or a negative error code on failure.
 */
BISON_API bison_error bison_rmi_proxy_call_oneway(
    bison_rmi_proxy proxy,
    bison_hash method_name,
    bison_handle params);

/**
 * @brief Register an event handler for a named server-initiated event.
 *
 * The handler is invoked on the client's worker thread when an event with
 * the given name is received for this proxy's remote object.
 *
 * @param proxy        Proxy handle.
 * @param event_name   Hashed event name token.
 * @param handler      Callback function (must not be NULL).
 * @param user         Opaque user pointer passed to the callback.
 * @return BISON_OK on success or a negative error code on failure.
 */
BISON_API bison_error bison_rmi_proxy_on_event(
    bison_rmi_proxy proxy,
    bison_hash event_name,
    bison_rmi_event_fn handler,
    void* user);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* BISON_C_H */
