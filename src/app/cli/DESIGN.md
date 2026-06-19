# CLI Application Design

## 1. Purpose and Scope

`src/app/cli` implements a standalone interactive REPL for connecting to and exploring
any bison RMI server. It is a developer diagnostics tool — not a scaffold for other
applications — analogous to a Python console or a Redis CLI, but for any bison RMI
server over any transport.

Source files owned by this directory:

- `cli_app.hpp` / `cli_app.cpp` — top-level class: argument parsing, transport
  construction, REPL loop, command dispatch.
- `main.cpp` — entry point.

This directory does NOT implement:

- Server logic or class registration — that lives in `src/rmi/server`.
- Transport internals — those live in `src/rmi/transport` and `src/app/pty`.

Cross-platform (Windows, Linux, macOS) except where the PTY transport is used
(Linux only, guarded with `#if defined(__linux__)`).

## 2. Design Goals

1. Connect to any bison server via any available transport without recompilation.
2. Allow interactive class discovery via `describe`.
3. Support the full object lifecycle: instantiate, get/set fields, call methods, destroy.
4. Accept and produce values as human-readable JSON.
5. Require no external readline library — use `std::getline` for portability.
6. Print results to stdout, errors to stderr, so output can be piped or scripted.

## 3. Key Abstractions

### cli_app

Single class owning the `rmi::client`, the handle table, and the REPL loop.

- `run(argc, argv) → int` — parses flags, builds transport, connects, enters the REPL,
  destroys all live handles, disconnects, returns exit code.
- `dispatch(line) → void` — tokenises one input line and routes to a command handler.
- `handle_table_` — `std::unordered_map<std::string, proxy::dynamic>` mapping
  auto-assigned names (`$1`, `$2`, …) to live proxy instances. `proxy::dynamic` is
  move-only; inserts use `try_emplace`, removals use `extract()`.
- `next_id_` — monotonically incrementing integer used to generate handle names.

### Transport factory (private helper in cli_app.cpp)

Reads the parsed argv flags and constructs the appropriate transport:

| Flag(s) | Transport constructed |
|---|---|
| `--host H --port P` (default) | `socket_client_transport{H, P}` |
| `--pipe CMD [ARGS…]` | `pipe_client_transport{CMD, ARGS}` |
| `--stdin` | `stream_client_transport{std::cin, std::cout}` |
| `--pty` (Linux only) | `pty_client_transport{}` |

The factory is not a separate class; it is a private helper method called once from
`run()` before `client.connect()`.

### JSON bridge (private helpers in cli_app.cpp)

Converts between `bison::dynamic` and JSON strings for input and output.

- **JSON → dynamic**: delegates to `bdg::bison::extensions::from_json(std::string)`
  declared in `src/bison/bison_object.hpp`. No new code required for this direction.
- **dynamic → JSON string**: a local `dynamic_to_json(const dynamic&) → std::string`
  helper maps bison primitives (monostate, bool, int32_t, float, string, key_t, nested
  dynamic, typed vectors) to a `nlohmann::json` value and serialises it with `dump(2)`
  for pretty-printing. `key_t` values are rendered as their registered string name when
  available, or `"#NNNN"` otherwise.

## 4. Data Flow and Architecture

### REPL loop

```
argv  ──parse──→ cli_app::run()
                    │
                    ├─ build transport (transport factory)
                    ├─ client.connect()
                    │
                    └─ loop:
                         std::getline(std::cin, line)   ← user types command
                         cli_app::dispatch(line)
                           ├─ tokenise
                           ├─ route to handler
                           │     ├─ client.describe(…).get()
                           │     ├─ client.instantiate(…).get() → handle $N
                           │     ├─ handle.get(…).get()
                           │     ├─ handle.set(…).get()
                           │     ├─ handle.call(…).get()
                           │     └─ client.destroy(std::move(handle))
                           └─ print result as JSON to stdout
                              (or error message to stderr)
                         EOF / "exit" → break
                    destroy all remaining handles
                    client.disconnect()
```

### Session lifecycle

```
INIT ──connect()──→ READY ──commands──→ READY
                                │
                            "exit"/EOF
                                │
                       destroy all handles
                            disconnect()
                                │
                              DONE
```

Connection errors during `connect()` exit before entering the REPL. Errors from
individual commands (e.g. `ERR_OBJECT_NOT_FOUND`, malformed JSON) are printed to stderr
and the REPL continues — they do not terminate the session. Only transport-level
failures after `connect()` propagate as fatal errors.

## 5. Public API Contract

### Command reference

All commands are case-insensitive. Angle brackets denote required tokens; square
brackets denote optional ones.

```
describe
```
Lists all classes registered on the server (namespace and class name, one per line).

```
describe <class>
```
Prints the fields and methods of the named class as pretty-printed JSON, including
display names, descriptions, and categories from the server's `OP_DESCRIBE` response.

```
new <namespace> <class> [json-params]
```
Instantiates an object on the server. `namespace` is a decimal integer (0 = global).
`json-params` is an optional JSON object passed as initialisation parameters; it
defaults to `{}`. Prints the assigned handle name (`$1`, `$2`, …) on success.

```
del <handle>
```
Destroys the named instance via `client.destroy()` and removes it from the handle table.

```
get <handle> [field ...]
```
Fetches fields from the remote object via `OP_GET`. Without field names, fetches all
fields. Prints the result as pretty-printed JSON.

```
set <handle> <json-object>
```
Updates fields via `OP_SET`. `json-object` must be a JSON object; its keys map to field
names on the remote object.

```
call <handle> <method> [json-params]
```
Invokes a method via `OP_CALL`. `json-params` defaults to `{}`. Prints the return value
as pretty-printed JSON.

```
list
```
Prints all active handles with their class names and object IDs.

```
help
```
Prints the command summary.

```
exit  |  quit  |  Ctrl+D
```
Destroys all live handles, disconnects, and exits with code 0.

### CLI options

```
bison-cli [--host HOST] [--port PORT]
          [--pipe CMD [ARGS...]]
          [--stdin]
          [--pty]           # Linux only
          [--timeout MS]    # per-request timeout, default 30000
```

Exactly one transport mode may be given: `--host`/`--port` (default: `localhost:7070`),
`--pipe`, `--stdin`, or `--pty`. Specifying more than one is a fatal argument error.

## 6. Design Decisions

**Standalone executable, not an extensible base class.**
`pty_server_app` and `pty_client_app` are scaffolds because applications need to inject
domain classes. The CLI has a fixed purpose — interactive exploration — so it does not
expose virtual hooks. An application that wants a custom REPL embeds `rmi::client`
directly.

**Handles named `$N` rather than user-chosen names.**
Auto-assignment keeps the command grammar simple and avoids quoting rules for names
containing special characters. The `$` prefix is familiar from shell conventions.
Users who need persistent names can alias them in their shell.

**JSON for all values.**
`bdg::bison::extensions::from_json()` already handles the JSON → dynamic direction
without new code. Only a local `dynamic_to_json` helper is needed for output.
JSON is human-readable and machine-parseable without a separate wire format.

**`std::getline` instead of GNU readline.**
Readline would require a `find_package` dependency the project does not currently carry
and is not available on all platforms. `std::getline` is portable and sufficient for a
diagnostics tool. History and tab-completion can be layered on later without changing
the core design.

**Errors continue the session.**
RMI errors (`ERR_OBJECT_NOT_FOUND`, `ERR_CLASS_NOT_FOUND`, etc.) are caught, printed to
stderr, and the REPL resumes. Only `connect()` failures and unrecoverable transport
errors terminate the process. This matches the behaviour of interactive interpreters
like Python and Redis CLI.

**PTY transport guarded at compile time.**
`--pty` is compiled only on Linux (`#if defined(__linux__)`). On other platforms the
flag is rejected with an error message at startup. This avoids maintaining a stub
implementation.

**`proxy::dynamic` stored directly in `std::unordered_map`.**
`proxy::dynamic` is move-only. Insertion uses `try_emplace`; removal uses `extract()`
to take ownership before passing to `client.destroy()`. This avoids wrapping in
`unique_ptr` and keeps the handle table straightforward.

## 7. Constraints and Invariants

- `cli_app` owns exactly one `rmi::client` for the lifetime of `run()`.
- All handles in `handle_table_` must be destroyed (via `client.destroy()`) before
  `client.disconnect()` is called. `run()` drains the table on every exit path.
- The REPL reads from `std::cin`; redirecting stdin enables non-interactive scripting.
- `bdg::bison::extensions::from_json()` is used for all JSON → dynamic conversions; it
  throws on malformed input or mixed-type arrays. The CLI catches these exceptions and
  prints to stderr without aborting the session.
- `--timeout MS` applies uniformly to all blocking `.get()` calls on request futures.
- `--pipe`, `--stdin`, and `--pty` are mutually exclusive with each other and with
  `--host`/`--port`; specifying more than one is a fatal argument error before `connect()`.
- On Linux, `--pty` requires the process to be running inside a PTY session where the
  server has already started `pty_server_transport` and is waiting for a HELLO frame.

## 8. Integration Boundaries

Depends on:

- `src/rmi/client/client` — `rmi::client` for all RPC operations (`connect`,
  `describe`, `instantiate`, `destroy`, `disconnect`).
- `src/rmi/proxy` — `proxy::dynamic` for object handles (`get`, `set`, `call`).
- `src/rmi/transport/socket_transport` — default TCP transport.
- `src/rmi/transport/pipe_transport` — subprocess pipe transport.
- `src/rmi/transport/stream_transport` — stdin/stdout stream transport.
- `src/app/pty/pty_client_transport` — PTY transport (Linux only).
- `src/bison/bison.hpp` — `bison::dynamic` and `bison::key_t` for runtime values.
- `src/bison/bison_object.hpp` — `bdg::bison::extensions::from_json()` for JSON-string
  → dynamic conversion.
- `extern/json` — `nlohmann::json` used in `dynamic_to_json` for output serialisation.

Depended on by:

- Nothing in `src/rmi` or `src/bison` depends on `src/app/cli`.
- The `bison-cli` binary produced from `main.cpp` in this directory.
