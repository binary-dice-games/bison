# Changelog

All notable user-facing changes to Bison are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `rmi::standalone::make_proxy(id)`: a proxy for an in-process object whose ID is already known, mirroring `rmi::client::make_proxy()`.
- `rmi::server::set_trace_payloads(bool)`: opt into decoded call payloads (`args=`, `set` values, response bodies) in request/response trace lines.
- `rmi::server::set_trace_lines(bool)`: when `false`, skip the request/response trace hooks entirely so the trace string is never formatted. Default `true` (unchanged behavior).
- Rust language bindings (`bindings/rust/`).
- Go language bindings (`bindings/go/`).
- `named_pipe_*_transport`: on Linux, a name starting with `@` is an abstract-namespace socket (no socket file, released when the process exits).
- `named_pipe_server_transport::start()` throws `address_in_use_error` when another endpoint already listens on the name.
- `rmi::client::is_connected()` and `bridge::upstream_connected()`: whether the (upstream) connection is still up.
- `named_pipe_client_transport::is_connected()`: reports when the server closed the connection, so an `rmi::client` over a pipe now notices a server that went away (`on_disconnect()`), as it already did over TCP.
- `app::bridge_app::make_upstream_transport()`: override to supply the upstream transport (e.g. an in-process `memory_client_transport`) instead of building it from the `--upstream_*` flags.

### Changed

- `rmi::server::on_after_dispatch` and `rmi::standalone::on_after_dispatch` now take a second `bison::key_t op` argument (the dispatched operation, `OP_DESTROY` for the session-teardown bracket) so overrides can distinguish read-only requests from mutating ones. Overrides must add the parameter.

- Request/response trace lines now omit decoded payloads by default, showing only envelope metadata (operation, session id, object id, method). Pass `--verbose=trace` to `bison-cli`/other server apps, or call `rmi::server::set_trace_payloads(true)`, to restore the full payloads.

- `--verbose` on server apps is now a verbosity level (`none` | `info` | `trace`, default `none`) instead of a boolean flag. `none` skips the trace hooks entirely, `info` prints envelope-metadata trace lines, `trace` also includes decoded payloads.

- `bison-cli` (and other client apps) now report a clear diagnostic when a connection fails because no RMI peer answered the handshake — including a hint to use `--transport=tcp`/`pipe`/`tls` when run outside a `--transport=term` host — instead of the internal `Worker thread exiting (code=...)` message.

### Fixed

- `rmi::client`: a request sent while the client was disconnecting could be left unresolved, blocking its `future::get()` forever; it now fails with a transport error.
- `--transport=pipe` (named-pipe / Unix-socket RMI transport): the server crashed on every incoming connection with a libuv assertion failure (`uv_accept: Assertion 'server->loop == client->loop' failed`).
- `named_pipe_client_transport::open()` no longer hangs when the connect fails immediately, and a failed `named_pipe_server_transport::start()` no longer leaks its event loop.
- `named_pipe_server_transport`: accepting a connection no longer touches freed stack memory.
- `named_pipe_server_transport`: `stop()` no longer frees state that a concurrent `accept()` (e.g. `rmi::server`'s accept loop during `server::stop()`) is still using, which could crash on shutdown; the transport can also be started again after `stop()`.
- `rmi::client`: after the server closed the connection, `disconnect()` now completes the teardown and `connect()` can start a new session; previously reconnecting called `std::terminate`.
- `rmi::server` now tears down the session of a client that went away without disconnecting (killed process, dropped connection) over TCP, TLS and pipes: their server connections report the peer's close through `is_closed()` once its frames are processed. Before, such sessions and their objects stayed until the server stopped.
- TCP and TLS server transports: accepting a connection no longer touches freed stack memory.
- `bridge`: the upstream client now notices the upstream server going away (the bridge's transport wrapper hid `is_connected()`).

## [1.0.0] - 2026-08-27

Initial release.

[1.0.0]: https://github.com/carloslopezmdez/bison/releases/tag/v1.0.0
