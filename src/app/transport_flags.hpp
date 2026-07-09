// MIT License © 2025 Binary Dice Games
/**
 * @file transport_flags.hpp
 * @brief Shared `--transport` / `--upstream_transport` flag handling for
 *        `server_app` / `client_app` / `bridge_app`.
 */
#pragma once

namespace bdg::bison::app {

/**
 * @brief Transport selected via `--transport` (or `--downstream_transport`
 *        for `bridge_app`).
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
 * Used by `server_app`/`client_app`, which each have a single transport and
 * no ambiguity to disambiguate. `bridge_app` uses
 * `selected_downstream_transport()` instead, since it has both a downstream
 * and an upstream transport active at once.
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
 * @brief Parses `FLAGS_downstream_transport` into a `transport_kind`.
 *
 * Mirrors `selected_transport()`, but reads `bridge_app`'s downstream-specific
 * flag set (`--downstream_transport`/`--host`/`--port`/`--name`). Named
 * distinctly from `selected_transport()` so a bridge's downstream and
 * upstream transport selectors are never confused with each other (e.g. in
 * `--help` output or in a caller's own flag docs).
 *
 * @return The selected downstream transport.
 * @throws std::runtime_error if `FLAGS_downstream_transport` is not one of
 *         "tcp", "pipe" or "term".
 */
transport_kind selected_downstream_transport();

/**
 * @brief Parses `FLAGS_upstream_transport` into a `transport_kind`.
 *
 * Mirrors `selected_downstream_transport()`, but reads `bridge_app`'s
 * upstream-specific flag set (`--upstream_transport`/`--upstream_host`/
 * `--upstream_port`/`--upstream_name`) instead of the downstream
 * `--downstream_transport`/`--host`/`--port`/`--name` flags.
 *
 * @return The selected upstream transport.
 * @throws std::runtime_error if `FLAGS_upstream_transport` is not one of
 *         "tcp", "pipe" or "term".
 */
transport_kind selected_upstream_transport();

} // namespace bdg::bison::app
