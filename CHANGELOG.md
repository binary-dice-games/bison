# Changelog

All notable user-facing changes to Bison are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `rmi::server::set_trace_payloads(bool)`: opt into decoded call payloads (`args=`, `set` values, response bodies) in request/response trace lines.
- `rmi::server::set_trace_lines(bool)`: when `false`, skip the request/response trace hooks entirely so the trace string is never formatted. Default `true` (unchanged behavior).
- Rust language bindings (`bindings/rust/`).
- Go language bindings (`bindings/go/`).

### Changed

- `rmi::server::on_after_dispatch` and `rmi::standalone::on_after_dispatch` now take a second `bison::key_t op` argument (the dispatched operation, `OP_DESTROY` for the session-teardown bracket) so overrides can distinguish read-only requests from mutating ones. Overrides must add the parameter.

- Request/response trace lines now omit decoded payloads by default, showing only envelope metadata (operation, session id, object id, method). Pass `--verbose=trace` to `bison-cli`/other server apps, or call `rmi::server::set_trace_payloads(true)`, to restore the full payloads.

- `--verbose` on server apps is now a verbosity level (`none` | `info` | `trace`, default `none`) instead of a boolean flag. `none` skips the trace hooks entirely, `info` prints envelope-metadata trace lines, `trace` also includes decoded payloads.

- `bison-cli` (and other client apps) now report a clear diagnostic when a connection fails because no RMI peer answered the handshake — including a hint to use `--transport=tcp`/`pipe`/`tls` when run outside a `--transport=term` host — instead of the internal `Worker thread exiting (code=...)` message.

### Fixed

- `--transport=pipe` (named-pipe / Unix-socket RMI transport): the server crashed on every incoming connection with a libuv assertion failure (`uv_accept: Assertion 'server->loop == client->loop' failed`).

## [1.0.0] - 2026-08-27

Initial release.

[1.0.0]: https://github.com/carloslopezmdez/bison/releases/tag/v1.0.0
