// MIT License © 2025 Binary Dice Games
/**
 * @file transport_flags.hpp
 * @brief Shared `--transport` flag handling for `server_app` / `client_app`.
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
  tcp,     ///< `--host`/`--port` TCP socket.
  pipe,    ///< `--name` named-pipe / Unix-socket path.
  pty,     ///< pty/stdio `BISON<...>` framing; no additional flags.
  console, ///< non-interactive stdio `BISON<...>` framing; server spawns `--cmd`.
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
 *         "pipe", "pty", or "console".
 */
transport_kind selected_transport();

} // namespace bdg::bison::app
