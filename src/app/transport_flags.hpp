// MIT License © 2025 Binary Dice Games
/**
 * @file transport_flags.hpp
 * @brief Shared `--transport` / `--upstream_transport` flag handling for
 *        `server_app` / `client_app` / `bridge_app`.
 */
#pragma once

namespace bdg::bison::app {

/**
 * @brief Transport selected via `--transport`.
 *
 * Exactly one transport is active per process; the flags relevant to the
 * other two are rejected by `enforce_transport_flag_exclusivity()`.
 */
enum class transport_kind {
  tcp, ///< `--host`/`--port` TCP socket.
  pipe, ///< `--name` named-pipe / Unix-socket path.
  term, ///< interactive pty/stdio OSC-99 framing (Linux/MSYS2 forkpty or Windows ConPTY); optional `--cmd`.
};

/**
 * @brief Parses `FLAGS_transport` into a `transport_kind`.
 *
 * Flags that don't apply to the selected transport (e.g. `--host` with
 * `--transport=pipe`) are simply ignored by the caller, which only reads
 * the flags relevant to `transport`.
 *
 * @return The selected transport.
 * @throws std::runtime_error if `FLAGS_transport` is not one of "tcp",
 *         "pipe" or "term".
 */
transport_kind selected_transport();

/**
 * @brief Parses `FLAGS_upstream_transport` into a `transport_kind`.
 *
 * Mirrors `selected_transport()`, but reads `bridge_app`'s upstream-specific
 * flag set (`--upstream_transport`/`--upstream_host`/`--upstream_port`/
 * `--upstream_name`) instead of the downstream `--transport`/`--host`/
 * `--port`/`--name` flags used by `server_app`.
 *
 * @return The selected upstream transport.
 * @throws std::runtime_error if `FLAGS_upstream_transport` is not one of
 *         "tcp", "pipe" or "term".
 */
transport_kind selected_upstream_transport();

} // namespace bdg::bison::app
