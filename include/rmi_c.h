// MIT License © 2025 Binary Dice Games
/**
 * @file rmi_c.h
 * @brief Pure-C public API for the Bison RMI framework.
 *
 * This header exposes remote method invocation capabilities as a stable C ABI
 * for building distributed object systems over network transports.  It is safe
 * to `#include` from C or C++ projects alike.
 *
 * ## Object lifecycle
 *
 * RMI handles are opaque tokens with reference-counting semantics:
 *
 * | Handle Type | Lifecycle |
 * |---|---|
 * | `rmi_client_handle` | Created by `rmi_client_tcp_create`, released by
 * `rmi_client_release` | | `rmi_server_handle` | Created by
 * `rmi_server_tcp_create`, released by `rmi_server_release` |
 *
 * ## Error handling
 *
 * Functions that can fail return `RMI_OK` on success or a negative `rmi_error`
 * code on failure.  Functions that return a handle return `NULL` on failure.
 * The library never throws exceptions across the C boundary; all C++ exceptions
 * are caught and converted to error codes.
 *
 * ## Thread safety
 *
 * The client's request-response loop runs on a background worker thread.
 * Multiple threads may invoke operations on the same client, though requests
 * are processed serially.  The server runs an accept loop on background
 * threads.
 *
 * ## Transport selection
 *
 * Socket (TCP), named-pipe / Unix-socket and terminal (spawned
 * subprocess) transports are all exposed via the regular client/server C
 * API. In-memory transport is available only through C++.
 */

#ifndef RMI_C_H
#define RMI_C_H

#include "bison_c.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Platform visibility macros (shared with bison_c.h) ────────────────── */

#if !defined(RMI_API)
#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef BISON_BUILDING_DLL
#define RMI_API __declspec(dllexport)
#else
#define RMI_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define RMI_API __attribute__((visibility("default")))
#else
#define RMI_API
#endif
#endif

/* ─── Opaque handles ────────────────────────────────────────────────────── */

/**
 * @brief Opaque handle to a client runtime.
 *
 * Never dereference the pointer directly; always use the `rmi_client_*` API
 * functions.  A null handle indicates an invalid or error state.
 */
typedef struct rmi_client_handle_* rmi_client_handle;

/**
 * @brief Opaque handle to a server runtime.
 *
 * Never dereference the pointer directly; always use the `rmi_server_*` API
 * functions.  A null handle indicates an invalid or error state.
 */
typedef struct rmi_server_handle_* rmi_server_handle;

/**
 * @brief Opaque handle to a remote object proxy.
 *
 * Wraps a server-side object reference issued by the server to a connecting
 * client.  A null handle indicates an invalid or error state.
 */
typedef struct rmi_proxy_handle_* rmi_proxy_handle;

/**
 * @brief Opaque handle to an asynchronous RMI operation.
 *
 * The handle wraps an internal future and is consumed by one of the
 * `rmi_future_get_*()` functions, or discarded with `rmi_future_release()`.
 */
typedef struct rmi_future_handle_* rmi_future_handle;

/* ─── Error codes ──────────────────────────────────────────────────────── */

/**
 * @brief Return codes used by RMI C API functions.
 *
 * Zero (`RMI_OK`) always means success.  Negative values are errors.
 */
typedef enum rmi_error {
  RMI_OK = 0, /**< Success. */
  RMI_ERR_NULL = -1, /**< A required handle or pointer argument was NULL. */
  RMI_ERR_INVALID_STATE = -2, /**< Operation invalid for current state (e.g., not connected). */
  RMI_ERR_TIMEOUT = -3, /**< Request timed out. */
  RMI_ERR_REMOTE_EXCEPTION = -4, /**< Server raised an exception. */
  RMI_ERR_TRANSPORT = -5, /**< Transport error (network, connection, etc.). */
  RMI_ERR_EXCEPTION = -6, /**< An unexpected C++ exception was caught. */
} rmi_error;

/* ─── Async future helpers ─────────────────────────────────────────────── */

/**
 * @brief Wait for an asynchronous operation to become ready.
 *
 * This does not consume the future handle.
 *
 * @param future     Valid async future handle.
 * @param timeout_ms Timeout in milliseconds, or -1 for the default timeout.
 * @return `RMI_OK` if the future is ready, or a negative error code.
 */
RMI_API rmi_error rmi_future_wait(rmi_future_handle future, int64_t timeout_ms);

/**
 * @brief Consume an async future that resolves to a dynamic result.
 *
 * On return, `*future` is set to `NULL` so the handle cannot be consumed
 * twice accidentally.
 *
 * @param future     Address of a valid async future handle.
 * @param out_value  Output bison handle; caller must release it with
 *                   `bison_release()`.
 * @return `RMI_OK` on success, or a negative error code.
 */
RMI_API rmi_error rmi_future_get_dynamic(rmi_future_handle* future, bison_handle* out_value);

/**
 * @brief Consume an async future that resolves to a proxy result.
 *
 * On return, `*future` is set to `NULL` so the handle cannot be consumed
 * twice accidentally.
 *
 * @param future      Address of a valid async future handle.
 * @param out_proxy   Output proxy handle; caller must release it with
 *                    `rmi_proxy_release()`.
 * @return `RMI_OK` on success, or a negative error code.
 */
RMI_API rmi_error rmi_future_get_proxy(rmi_future_handle* future, rmi_proxy_handle* out_proxy);

/**
 * @brief Release an async future without consuming its result.
 *
 * @param future  Future handle to release. A `NULL` handle is ignored.
 */
RMI_API void rmi_future_release(rmi_future_handle future);

/* ═══════════════════════════════════════════════════════════════════════════
 * Client
 * ═════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Create a TCP socket client.
 *
 * The client is not connected until `rmi_client_connect()` is called.
 *
 * @param host    Server hostname or IP address (e.g., "127.0.0.1").
 * @param port    Server port number (0-65535).
 * @return New client handle, or `NULL` on allocation failure.
 *
 * @code{.c}
 * rmi_client_handle client = rmi_client_tcp_create("127.0.0.1", 8080);
 * // ... use client ...
 * rmi_client_release(client);
 * @endcode
 */
RMI_API rmi_client_handle rmi_client_tcp_create(const char* host, uint16_t port);

/**
 * @brief Create named-pipe / Unix-socket client.
 *
 * On Windows, @p path is a full pipe path (`\\.\pipe\name`). On Linux/macOS,
 * @p path is a file-system socket path (e.g. `/tmp/wish.sock`).
 *
 * @param path Pipe or Unix-socket path to connect to.
 * @return New client handle, or `NULL` on allocation failure.
 */
RMI_API rmi_client_handle rmi_client_pipe_create(const char* path);

/**
 * @brief Create a terminal (OSC-99 framed) client.
 *
 * Wraps the calling process's own inherited stdio (fd 0 for reads, fd 1 for
 * writes) with the term transport. Intended for a client process that is
 * itself running as the child spawned by `rmi_server_term_create()`.
 *
 * @return New client handle, or `NULL` on allocation failure.
 */
RMI_API rmi_client_handle rmi_client_term_create(void);

/**
 * @brief Create a standalone in-process client.
 *
 * The returned handle can be used with all existing `rmi_client_*` and
 * `rmi_proxy_*` API functions. `rmi_client_connect()` and
 * `rmi_client_disconnect()` are accepted but are no-ops for standalone
 * handles.
 *
 * @return New standalone client handle, or `NULL` on allocation failure.
 */
RMI_API rmi_client_handle rmi_standalone_create(void);

/**
 * @brief Connect the client to the server.
 *
 * Opens the transport and starts the background worker thread for receiving
 * responses.
 *
 * @param client    Valid client handle.
 * @param params    Optional connection parameters (`bison_handle` or `NULL`).
 * @return `RMI_OK` on success, or a negative error code.
 */
RMI_API rmi_error rmi_client_connect(rmi_client_handle client, bison_handle params);

/**
 * @brief Request class metadata from the server.
 *
 * @param client    Valid connected client handle.
 * @param ns        Namespace key to query, or `0` for the global namespace.
 * @param klass     Class key to query, or `0` for all metadata.
 * @param out_desc  Output handle for the description object; caller must
 *                  release this with `bison_release()`.
 * @return `RMI_OK` on success, or a negative error code.
 */
RMI_API rmi_error
rmi_client_describe(rmi_client_handle client, bison_hash ns, bison_hash klass, bison_handle* out_desc);

/**
 * @brief Request class metadata from the server asynchronously.
 *
 * @param client      Valid connected client handle.
 * @param ns          Namespace key to query, or `0` for the global namespace.
 * @param klass       Class key to query, or `0` for all metadata.
 * @param out_future  Output async future consumed with
 *                    `rmi_future_get_dynamic()`.
 * @return `RMI_OK` on successful submission, or a negative error code.
 */
RMI_API rmi_error
rmi_client_describe_async(rmi_client_handle client, bison_hash ns, bison_hash klass, rmi_future_handle* out_future);

/**
 * @brief Instantiate a remote object on the server.
 *
 * @param client    Valid connected client handle.
 * @param ns        Namespace key to instantiate in, or `0` for global.
 * @param klass     Class key to instantiate (use `bison_key()` to compute).
 * @param params    Constructor parameters (`bison_handle` or `NULL`).
 * @param out_proxy Output proxy handle for the remote object; the caller owns
 *                  this and must release it with `rmi_proxy_release()`.
 * @return `RMI_OK` on success, or a negative error code.
 */
RMI_API rmi_error rmi_client_instantiate(
    rmi_client_handle client,
    bison_hash ns,
    bison_hash klass,
    bison_handle params,
    rmi_proxy_handle* out_proxy);

/**
 * @brief Instantiate a remote object asynchronously.
 *
 * @param client      Valid connected client handle.
 * @param ns          Namespace key to instantiate in, or `0` for global.
 * @param klass       Class key to instantiate.
 * @param params      Constructor parameters (`bison_handle` or `NULL`).
 * @param out_future  Output async future consumed with
 *                    `rmi_future_get_proxy()`.
 * @return `RMI_OK` on successful submission, or a negative error code.
 */
RMI_API rmi_error rmi_client_instantiate_async(
    rmi_client_handle client,
    bison_hash ns,
    bison_hash klass,
    bison_handle params,
    rmi_future_handle* out_future);

/**
 * @brief Disconnect from the server and stop worker threads.
 *
 * @param client    Valid client handle.
 * @return `RMI_OK` on success, or a negative error code.
 */
RMI_API rmi_error rmi_client_disconnect(rmi_client_handle client);

/**
 * @brief Release a client handle.
 *
 * If the client is connected, it will be disconnected first.  @p client must
 * not be used after calling this function.
 *
 * @param client    Client handle to release.  A `NULL` @p client is silently
 * ignored.
 */
RMI_API void rmi_client_release(rmi_client_handle client);

/* ─── Proxy lifecycle ──────────────────────────────────────────────────── */

/**
 * @brief Release a proxy handle.
 *
 * This sends a destroy request to the server for the associated remote object.
 *
 * @param proxy     Proxy handle to release.  A `NULL` @p proxy is silently
 * ignored.
 */
RMI_API void rmi_proxy_release(rmi_proxy_handle proxy);

/**
 * @brief Callback invoked when a proxy event is received from the server.
 *
 * @param params  Read-only event parameters. The handle is valid only during
 *                callback execution and must not be released by the callback.
 * @param user    User context pointer supplied to `rmi_proxy_on_event()`.
 */
typedef void (*rmi_proxy_event_fn)(bison_handle params, void* user);

/**
 * @brief Subscribe to a server-initiated event on a remote proxy.
 *
 * @param proxy       Valid proxy handle.
 * @param event_name  Event name hash (use `bison_key()` to compute).
 * @param handler     Event callback function.
 * @param user        User context pointer passed to @p handler.
 * @return `RMI_OK` on success, or a negative error code.
 */
RMI_API rmi_error
rmi_proxy_on_event(rmi_proxy_handle proxy, bison_hash event_name, rmi_proxy_event_fn handler, void* user);

/**
 * @brief Clear explicitly set fields on a remote object.
 *
 * @param proxy      Valid proxy handle.
 * @param timeout_ms Timeout in milliseconds, or -1 for the default timeout.
 * @return `RMI_OK` on success, or a negative error code.
 */
RMI_API rmi_error rmi_proxy_clear(rmi_proxy_handle proxy, int64_t timeout_ms);

/**
 * @brief Clear explicitly set fields on a remote object asynchronously.
 *
 * @param proxy       Valid proxy handle.
 * @param out_future  Output async future waited on with
 *                    `rmi_future_wait()` and released with
 *                    `rmi_future_release()`.
 * @return `RMI_OK` on successful submission, or a negative error code.
 */
RMI_API rmi_error rmi_proxy_clear_async(rmi_proxy_handle proxy, rmi_future_handle* out_future);

/**
 * @brief Apply a partial field update to a remote object.
 *
 * @param proxy      Valid proxy handle.
 * @param fields     Field patch object as a `bison_handle`, or `NULL`.
 * @param timeout_ms Timeout in milliseconds, or -1 for the default timeout.
 * @return `RMI_OK` on success, or a negative error code.
 */
RMI_API rmi_error rmi_proxy_set(rmi_proxy_handle proxy, bison_handle fields, int64_t timeout_ms);

/**
 * @brief Apply a partial field update to a remote object asynchronously.
 *
 * @param proxy       Valid proxy handle.
 * @param fields      Field patch object as a `bison_handle`, or `NULL`.
 * @param out_future  Output async future waited on with
 *                    `rmi_future_wait()` and released with
 *                    `rmi_future_release()`.
 * @return `RMI_OK` on successful submission, or a negative error code.
 */
RMI_API rmi_error rmi_proxy_set_async(rmi_proxy_handle proxy, bison_handle fields, rmi_future_handle* out_future);

/**
 * @brief Retrieve fields from a remote object.
 *
 * @param proxy       Valid proxy handle.
 * @param projection  Optional projection object as a `bison_handle`, or
 *                    `NULL` for a full snapshot.
 * @param out_result  Output handle for the retrieved object; caller must
 *                    release it with `bison_release()`.
 * @param timeout_ms  Timeout in milliseconds, or -1 for the default timeout.
 * @return `RMI_OK` on success, or a negative error code.
 */
RMI_API rmi_error
rmi_proxy_get(rmi_proxy_handle proxy, bison_handle projection, bison_handle* out_result, int64_t timeout_ms);

/**
 * @brief Retrieve fields from a remote object asynchronously.
 *
 * @param proxy       Valid proxy handle.
 * @param projection  Optional projection object as a `bison_handle`, or
 *                    `NULL` for a full snapshot.
 * @param out_future  Output async future consumed with
 *                    `rmi_future_get_dynamic()`.
 * @return `RMI_OK` on successful submission, or a negative error code.
 */
RMI_API rmi_error rmi_proxy_get_async(rmi_proxy_handle proxy, bison_handle projection, rmi_future_handle* out_future);

/**
 * @brief Call a method on a remote object.
 *
 * @param proxy      Valid proxy handle.
 * @param method     Method name hash (use `bison_key()` to compute).
 * @param params     Method arguments as a `bison_handle` object, or `NULL`.
 * @param out_result Output result object; caller must release with
 * `bison_release()`, or `NULL` to ignore the result.
 * @param timeout_ms Timeout in milliseconds, or -1 for no timeout.
 * @return `RMI_OK` on success, or a negative error code.
 */
RMI_API rmi_error rmi_proxy_call(
    rmi_proxy_handle proxy,
    bison_hash method,
    bison_handle params,
    bison_handle* out_result,
    int64_t timeout_ms);

/**
 * @brief Call a method on a remote object asynchronously.
 *
 * @param proxy       Valid proxy handle.
 * @param method      Method name hash.
 * @param params      Method arguments as a `bison_handle`, or `NULL`.
 * @param out_future  Output async future consumed with
 *                    `rmi_future_get_dynamic()`.
 * @return `RMI_OK` on successful submission, or a negative error code.
 */
RMI_API rmi_error
rmi_proxy_call_async(rmi_proxy_handle proxy, bison_hash method, bison_handle params, rmi_future_handle* out_future);

/* ═══════════════════════════════════════════════════════════════════════════
 * Server
 * ═════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Create a TCP socket server listener.
 *
 * The server is not listening until `rmi_server_listen()` is called.
 *
 * @param host    Bind address (e.g., "0.0.0.0" for all interfaces, or
 * "127.0.0.1" for localhost).
 * @param port    Bind port number.
 * @return New server handle, or `NULL` on allocation failure.
 *
 * @code{.c}
 * rmi_server_handle server = rmi_server_tcp_create("0.0.0.0", 8080);
 * // ... use server ...
 * rmi_server_release(server);
 * @endcode
 */
RMI_API rmi_server_handle rmi_server_tcp_create(const char* host, uint16_t port);

/**
 * @brief Create named-pipe / Unix-socket server listener.
 *
 * The server is not listening until `rmi_server_listen()` is called.
 *
 * On Windows, @p path is a full pipe path (`\\.\pipe\name`). On Linux/macOS,
 * @p path is a file-system socket path (e.g. `/tmp/wish.sock`).
 *
 * @param path Pipe or Unix-socket path to bind.
 * @return New server handle, or `NULL` on allocation failure.
 */
RMI_API rmi_server_handle rmi_server_pipe_create(const char* path);

/**
 * @brief Create a terminal (OSC-99 framed) server listener.
 *
 * Spawns a child process attached to a new pseudo-terminal and wires the
 * term transport to its pty I/O; the calling process's own real input is
 * pumped into the pty for the lifetime of the server. The server is not
 * listening until `rmi_server_listen()` is called.
 *
 * @param cmd Command to exec in the spawned child, or `NULL`/empty to spawn
 *            the operator's default shell.
 * @return New server handle, or `NULL` on allocation failure.
 *
 * @code{.c}
 * rmi_server_handle server = rmi_server_term_create(NULL);
 * // ... use server ...
 * rmi_server_release(server);
 * @endcode
 */
RMI_API rmi_server_handle rmi_server_term_create(const char* cmd);

/**
 * @brief Start the server listener.
 *
 * Begins accepting client connections on the configured address/port and
 * spawns background worker threads.
 *
 * @param server    Valid server handle.
 * @param params    Optional listen parameters (`bison_handle` or `NULL`).
 * @return `RMI_OK` on success, or a negative error code.
 */
RMI_API rmi_error rmi_server_listen(rmi_server_handle server, bison_handle params);

/**
 * @brief Stop the server listener.
 *
 * Closes the listener socket, stops accepting new connections, and allows
 * active worker threads to complete.
 *
 * @param server    Valid server handle.
 */
RMI_API void rmi_server_stop(rmi_server_handle server);

/**
 * @brief Release a server handle.
 *
 * If the server is listening, it will be stopped first.  @p server must not
 * be used after calling this function.
 *
 * @param server    Server handle to release.  A `NULL` @p server is silently
 * ignored.
 */
RMI_API void rmi_server_release(rmi_server_handle server);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // RMI_C_H
