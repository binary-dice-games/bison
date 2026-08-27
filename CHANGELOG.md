# Changelog

All notable user-facing changes to Bison are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- `bison-cli` (and other client apps) now report a clear diagnostic when a connection fails because no RMI peer answered the handshake — including a hint to use `--transport=tcp`/`pipe`/`tls` when run outside a `--transport=term` host — instead of the internal `Worker thread exiting (code=...)` message.

## [1.0.0] - 2026-08-27

Initial release.

[1.0.0]: https://github.com/carloslopezmdez/bison/releases/tag/v1.0.0
