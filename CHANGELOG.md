# Changelog

All notable user-facing changes to Bison are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `rmi::server::set_trace_payloads(bool)`: opt into decoded call payloads (`args=`, `set` values, response bodies) in request/response trace lines.
- `rmi::server::set_trace_lines(bool)`: when `false`, skip the request/response trace hooks entirely so the trace string is never formatted. Default `true` (unchanged behavior).
- Rust language bindings (`bindings/rust/`).

### Changed

- Request/response trace lines now omit decoded payloads by default, showing only envelope metadata (operation, session id, object id, method). Pass `--verbose=trace` to `bison-cli`/other server apps, or call `rmi::server::set_trace_payloads(true)`, to restore the full payloads.

- `--verbose` on server apps is now a verbosity level (`none` | `info` | `trace`, default `none`) instead of a boolean flag. `none` skips the trace hooks entirely, `info` prints envelope-metadata trace lines, `trace` also includes decoded payloads.

- `bison-cli` (and other client apps) now report a clear diagnostic when a connection fails because no RMI peer answered the handshake — including a hint to use `--transport=tcp`/`pipe`/`tls` when run outside a `--transport=term` host — instead of the internal `Worker thread exiting (code=...)` message.

## [1.0.0] - 2026-08-27

Initial release.

[1.0.0]: https://github.com/carloslopezmdez/bison/releases/tag/v1.0.0
