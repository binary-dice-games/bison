// MIT License © 2025 Binary Dice Games
/**
 * @file pty_c.h
 * @brief Pure-C public API for the Bison PTY application scaffold.
 *
 * Exposes `pty_client_app` and `pty_server_app` as a stable C ABI so that
 * host processes written in any language can drive a PTY-backed RMI session
 * without linking C++ directly.
 *
 * Linux only — on other platforms both run functions return
 * `RMI_ERR_INVALID_STATE`.
 *
 * ## Usage pattern
 *
 * Zero-initialise the callback struct, fill in at minimum the required
 * callback, and call the run function.  All optional fields that remain NULL
 * fall back to the base-class default behaviour.
 *
 * @code{.c}
 * #include "pty_c.h"
 *
 * static int my_session(rmi_client_handle client, void* user) {
 *     // call rmi_client_* and rmi_proxy_* here
 *     return 0;
 * }
 *
 * int main(int argc, char** argv) {
 *     rmi_pty_client_callbacks cb = {0};
 *     cb.on_session = my_session;
 *     return rmi_pty_client_run(argc, argv, &cb) == RMI_OK ? 0 : 1;
 * }
 * @endcode
 */

#ifndef PTY_C_H
#define PTY_C_H

#include "rmi_c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── PTY client callbacks ─────────────────────────────────────────────────── */

/**
 * @brief Invoked during the PTY client session.
 *
 * @p client is a non-owning handle valid only for the duration of the
 * callback.  Use `rmi_client_*` and `rmi_proxy_*` APIs to interact with the
 * server.  Do not retain or release this handle.
 *
 * @param client  Connected client handle (callback lifetime only).
 * @param user    User context pointer from `rmi_pty_client_callbacks`.
 * @return Exit code: `0` maps to `RMI_OK`; non-zero maps to an error.
 */
typedef int (*rmi_pty_client_on_session_fn)(
    rmi_client_handle client,
    void* user);

/**
 * @brief Invoked immediately after the handshake succeeds, before `on_session`.
 *
 * @param user  User context pointer from `rmi_pty_client_callbacks`.
 */
typedef void (*rmi_pty_client_on_connected_fn)(void* user);

/**
 * @brief Invoked when the PTY client catches a transport or session error.
 *
 * @param message  Error description, valid only during the callback.
 * @param user     User context pointer from `rmi_pty_client_callbacks`.
 */
typedef void (*rmi_pty_client_on_error_fn)(const char* message, void* user);

/**
 * @brief Invoked before `connect()` to populate transport parameters.
 *
 * @p params is an in/out bison handle representing the parameter map.
 * Use `bison_set_*` APIs to add or override keys.  Do not release @p params.
 * Default values set by the base class: `mode=dcs`,
 * `handshake_timeout_ms=300000`.
 *
 * @param params  Mutable parameter map (borrowed, callback lifetime only).
 * @param user    User context pointer from `rmi_pty_client_callbacks`.
 */
typedef void (*rmi_pty_client_on_connect_params_fn)(
    bison_handle params,
    void* user);

/**
 * @brief Callbacks for `rmi_pty_client_run()`.
 *
 * Zero-initialise this struct and set at minimum `on_session`.  Unused
 * optional fields (NULL) fall back to the base-class default behaviour.
 *
 * @code{.c}
 * rmi_pty_client_callbacks cb = {0};
 * cb.on_session = my_session_fn;
 * cb.user       = my_context;
 * rmi_pty_client_run(argc, argv, &cb);
 * @endcode
 */
typedef struct rmi_pty_client_callbacks {
  rmi_pty_client_on_session_fn on_session;               /**< Required. */
  rmi_pty_client_on_connected_fn on_connected;           /**< Optional. */
  rmi_pty_client_on_error_fn on_error;                   /**< Optional. */
  rmi_pty_client_on_connect_params_fn on_connect_params; /**< Optional. */
  void* user;
} rmi_pty_client_callbacks;

/**
 * @brief Run the PTY RMI client application.
 *
 * Wraps `bdg::bison::app::pty_client_app`.  Blocks until the session ends.
 * On non-Linux platforms returns `RMI_ERR_INVALID_STATE`.
 *
 * @param argc       Argument count.
 * @param argv       Argument vector.
 * @param callbacks  Callback table; `on_session` must be non-NULL.
 * @return `RMI_OK` on success, or a negative error code.
 */
RMI_API rmi_error rmi_pty_client_run(
    int argc,
    char** argv,
    const rmi_pty_client_callbacks* callbacks);

/* ── PTY server callbacks ─────────────────────────────────────────────────── */

/**
 * @brief Invoked once before the session loop to register domain classes.
 *
 * @param user  User context pointer from `rmi_pty_server_callbacks`.
 */
typedef void (*rmi_pty_server_register_classes_fn)(void* user);

/**
 * @brief Invoked to obtain the shell command launched via `forkpty`.
 *
 * Return a pointer to a null-terminated string (e.g. `"bash"`).  The string
 * must remain valid for the duration of the callback.  Return `NULL` to use
 * the default (`"bash"`).
 *
 * @param user  User context pointer from `rmi_pty_server_callbacks`.
 * @return Shell command string, or `NULL` for the default.
 */
typedef const char* (*rmi_pty_server_shell_command_fn)(void* user);

/**
 * @brief Invoked to obtain transport parameters passed to `start()`.
 *
 * Return a newly created `bison_handle` populated with the desired parameters.
 * The runtime releases the returned handle after extracting the values.
 * Return `NULL` to use the default (`mode=dcs`).
 *
 * @param user  User context pointer from `rmi_pty_server_callbacks`.
 * @return Owned bison handle with parameters, or `NULL` for the default.
 */
typedef bison_handle (*rmi_pty_server_listen_params_fn)(void* user);

/**
 * @brief Invoked once after a client connects and the session server is ready.
 *
 * @param user  User context pointer from `rmi_pty_server_callbacks`.
 */
typedef void (*rmi_pty_server_on_client_connected_fn)(void* user);

/**
 * @brief Invoked after each session ends and the session server is destroyed.
 *
 * @param user  User context pointer from `rmi_pty_server_callbacks`.
 */
typedef void (*rmi_pty_server_on_session_ended_fn)(void* user);

/**
 * @brief Invoked when the PTY server catches a transport-level error.
 *
 * @param message  Error description, valid only during the callback.
 * @param user     User context pointer from `rmi_pty_server_callbacks`.
 */
typedef void (*rmi_pty_server_on_error_fn)(const char* message, void* user);

/**
 * @brief Callbacks for `rmi_pty_server_run()`.
 *
 * Zero-initialise this struct and set at minimum `register_classes`.  Unused
 * optional fields (NULL) fall back to the base-class default behaviour.
 *
 * @code{.c}
 * rmi_pty_server_callbacks cb = {0};
 * cb.register_classes = my_register_fn;
 * cb.user             = my_context;
 * rmi_pty_server_run(argc, argv, &cb);
 * @endcode
 */
typedef struct rmi_pty_server_callbacks {
  rmi_pty_server_register_classes_fn register_classes;       /**< Required. */
  rmi_pty_server_shell_command_fn shell_command;             /**< Optional. */
  rmi_pty_server_listen_params_fn listen_params;             /**< Optional. */
  rmi_pty_server_on_client_connected_fn on_client_connected; /**< Optional. */
  rmi_pty_server_on_session_ended_fn on_session_ended;       /**< Optional. */
  rmi_pty_server_on_error_fn on_error;                       /**< Optional. */
  void* user;
} rmi_pty_server_callbacks;

/**
 * @brief Run the PTY RMI server application.
 *
 * Wraps `bdg::bison::app::pty_server_app`.  Blocks until the shell subprocess
 * exits or a fatal error occurs.  On non-Linux platforms returns
 * `RMI_ERR_INVALID_STATE`.
 *
 * @param argc       Argument count.
 * @param argv       Argument vector.
 * @param callbacks  Callback table; `register_classes` must be non-NULL.
 * @return `RMI_OK` on success, or a negative error code.
 */
RMI_API rmi_error rmi_pty_server_run(
    int argc,
    char** argv,
    const rmi_pty_server_callbacks* callbacks);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // PTY_C_H
