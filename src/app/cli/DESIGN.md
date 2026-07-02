# CLI Application Design

## 1. Purpose and Scope

`src/app/cli` implements an extensible interactive REPL base class for connecting
to and exploring any bison RMI server.  It uses a scripting-style syntax:

```
> t = instantiate("Ikea", "Table")
> t.get({"material": null})
{ "material": "wood" }
> t.set({"material": "iron"})
> t.call("flip", {})
> del t
> exit
```

Source files owned by this directory:

- `cli_app.hpp` / `cli_app.cpp` — extensible base class: argument parsing,
  transport construction, REPL loop, command dispatch.
- `main.cpp` — entry point for the standalone `bison-cli` binary.

This directory does NOT implement:

- Server logic or class registration — that lives in `src/rmi/server`.
- Transport internals — those live in `src/rmi/transport` and `src/pty`.

Cross-platform (Windows, Linux, macOS)

## 2. Design Goals

1. Connect to any bison server via any available transport without recompilation.
2. Allow interactive class discovery via `describe` and `info`.
3. Support the full object lifecycle: instantiate, get/set fields, call methods, destroy.
4. Accept and produce values as JSON.
5. Require no external readline library — use `std::getline` for portability.
6. Print results to stdout, errors to stderr, so output can be piped or scripted.
7. Be extensible: downstream projects (e.g. a terminal UI) subclass `cli_app`
   and override `on_session()` to replace the REPL with a custom session.

## 3. Key Abstractions

### cli_app

Extensible base class owning the REPL lifecycle.  Extends `client_app`
(`src/app/client/client_app.hpp`), inheriting its flag parsing and
transport-selection logic (including `--pty`).

- `run(argc, argv) → int` — non-virtual; parses flags, builds transport,
  calls `connect()`, calls `on_session()`, drains all live proxies, calls
  `disconnect()`, returns exit code.
- `on_session(rmi::client&) → int` — virtual with a default implementation
  (the interactive REPL).  Override to replace the REPL entirely.
- `on_connected()` — virtual hook called after handshake; default: no-op.
- `on_connect_params(dynamic&)` — virtual hook to customise connection params.
- `on_error(string)` — virtual hook for fatal error reporting.
- `timeout_` — protected member set from `--timeout`; the default REPL uses
  it for per-request `std::future::wait_for()`.

### Variable table (inside `on_session`)

`std::unordered_map<std::string, proxy::dynamic>` maps user-chosen names to
live proxy instances.  Lives as a local inside `on_session()` so subclasses
that override the method carry no unused state.

### Name resolution (inside `on_session`)

`std::unordered_map<uint32_t, std::string>` maps `key_t` hash IDs to
human-readable strings.  Used by `dynamic_to_json()` to emit readable field
names instead of `#NNNNNN` hash fallbacks.

The map is populated from three sources, applied in order:

1. **Protocol constants** — hard-coded in `make_known_keys()` at session start;
   covers all `__`-prefixed wire-protocol field names (`__name`, `__fields`,
   `__displayName`, etc.).
2. **Server dictionary** — `get_dictionary()` is called once after `connect()`.
   The server walks its class registry and returns a flat `bison::dynamic`
   mapping each `DisplayName`-annotated item's `key_t` hash to its display-name
   string.  The result is merged into the map via `merge_dictionary()`.  The
   fetch is non-fatal: if the server does not support `OP_DICTIONARY` the map
   continues with protocol constants only.
3. **User-typed names** — whenever the user types a namespace, class, or method
   name in a REPL command, `register_key()` hashes it with `key_t{name}` and
   stores it.  This handles names that lack `DisplayName` attributes.

Note: `key_t` is a one-way FNV-1a hash, so the server cannot recover original
string names for items without `DisplayName`.  Only annotated items appear in
the server dictionary; unannotated hashes still fall back to `#NNNNNN`.

### Transport factory (private helper in cli_app.cpp)

Reads the parsed argv flags and constructs the appropriate transport:

| Flag(s) | Transport constructed |
|---|---|
| `--host H --port P` (default) | `socket_client_transport{H, P}` |
| `--pipe PATH` | `named_pipe_client_transport{PATH}` |
| `--pty` | `stdio_client_transport{0, 1}` — wraps this process's own inherited stdio, cross-platform; no subprocess or pty is spawned client-side. Also puts fd 0 in raw mode (`pty::raw_mode_guard`, Linux-only) and routes REPL input through `client_app::read_console_line()` instead of `std::cin` — see `src/pty/DESIGN.md` for why fd 0 can't be read directly in this mode |

### JSON bridge (private helpers in cli_app.cpp)

Converts between `bison::dynamic` and JSON for input and output.

- **JSON → dynamic**: `bdg::bison::extensions::from_json(string)` declared in
  `src/bison/bison_object.hpp`.
- **dynamic → JSON**: local `dynamic_to_json(dynamic, key_name_map)` converts
  each field using `std::visit` on `field_base`.  Field keys are resolved
  through the name-resolution map; unresolved hashes render as `"#NNNN"`.
- `unquote(string_view)` strips JSON string delimiters from user-supplied
  arguments using `nlohmann::json::parse`.

## 4. Data Flow and Architecture

### REPL loop

```
argv  ──parse──→ cli_app::run()
                    │
                    ├─ build transport
                    ├─ client.connect()
                    ├─ client.get_dictionary() → merge into km
                    │
                    └─ on_session(c)   ← override to replace REPL
                         │
                         └─ loop:
                              read_console_line(line)  ← std::cin, or the
                                                          --pty passthrough
                                                          queue (see client_app)
                              dispatch(line, c, handles, km, timeout)
                                ├─ name = instantiate(...) → proxy → handles
                                ├─ name.get([projection])  → JSON to stdout
                                ├─ name.set({fields})
                                ├─ name.call("method", {}) → JSON to stdout
                                ├─ del name  → client.destroy()
                                ├─ describe[("Ns","Cls")] → JSON to stdout
                                ├─ info      → client.get_help() → text to stdout
                                ├─ list / help
                                └─ exit / EOF → break
                         drain handles (destroy all)
                    client.disconnect()
```

### Session lifecycle

```
INIT ──connect()──→ get_dictionary()──→ READY ──commands──→ READY
                                                    │
                                                exit/EOF
                                                    │
                                           destroy all proxies
                                              disconnect()
                                                    │
                                                  DONE
```

Connection errors during `connect()` exit before entering the REPL. Errors
from individual commands are printed to stderr and the REPL continues. Only
transport-level failures after `connect()` propagate as fatal errors.

## 5. Command Reference

All commands are case-sensitive.

```
name = instantiate("namespace", "Class")
name = instantiate("namespace", "Class", {json-params})
```
Instantiates a server-side object. Prints the assigned name on success.

```
name.get()
name.get({"field": null, ...})
```
Retrieves fields from the remote object. Without a projection, retrieves all
fields. Prints result as pretty-printed JSON.

```
name.set({"field": value, ...})
```
Applies a partial field update to the remote object.

```
name.call("method")
name.call("method", {json-params})
```
Invokes a method. Prints the return value as pretty-printed JSON.

```
del name
```
Destroys the named instance and removes it from the variable table.

```
describe
describe("namespace", "Class")
```
Without arguments, lists all classes registered on the server. With arguments,
prints the field and method schema for the named class.

```
info
```
Prints human-readable help text returned by the server (`OP_HELP`): a summary
of the server's purpose and the remote classes it exports, including display
names and descriptions.

```
list
```
Prints all active variable names with their object IDs.

```
help
```
Prints the command summary.

```
exit  |  quit  |  Ctrl+D
```
Destroys all live proxies, disconnects, and exits with code 0.

## 6. CLI Options

```
bison-cli [--host HOST] [--port PORT]
          [--pipe PATH]
          [--pty]
          [--timeout MS]    # per-request timeout, default 30000
```

Exactly one transport mode may be given: `--host`/`--port` (default:
`localhost:7070`), `--pipe`, or `--pty`. `--pty` takes precedence over
`--pipe`, which takes precedence over `--host`/`--port`.

## 7. Design Decisions

**Extensible base class, not a standalone executable.**
`server_app` and `client_app` are scaffolds because downstream
projects need to inject domain logic.  `cli_app` follows the same pattern:
the REPL is the default `on_session()` behaviour, and projects like "wish"
can override it to render a terminal UI driven by the same RMI connection.
The standalone `bison-cli` binary is simply `main()` with a bare `cli_app`.

**Named variables instead of `$N` handles.**
User-chosen names (`my_table`) are more readable than auto-assigned ordinals
(`$1`) and make sessions with multiple live instances easier to follow.

**Dot-notation method calls.**
`my_table.get(...)` reads as natural object access.  It keeps the REPL syntax
consistent with how proxies are used in C++ code.

**JSON for all values.**
`bdg::bison::extensions::from_json()` already handles JSON → dynamic without
new code.  Output uses `nlohmann::json::dump(2)` for pretty-printing.  JSON
is human-readable and machine-parseable for scripting.

**`std::getline` instead of GNU readline.**
Readline requires a `find_package` dependency the project does not carry and
is not available on all platforms.  `std::getline` is portable and sufficient.
History and tab-completion can be added later without changing the core design.
(In `--pty` mode, `client_app::read_console_line()` reads from a passthrough
queue instead of `std::cin` directly — see the `--pty` note below — but it's
still a plain blocking line read from `dispatch()`'s point of view.)

**Per-request timeout via `std::future::wait_for`.**
The `--timeout` flag sets `timeout_` before `on_session()` is called.  Each
blocking `.get()` call uses `wait_for(timeout_)` so a hung server does not
block the REPL indefinitely.

**Errors continue the session.**
RMI errors (`ERR_OBJECT_NOT_FOUND`, timeouts, etc.) are caught, printed to
stderr, and the REPL resumes.  Only `connect()` failures and unrecoverable
transport errors terminate the process.

**`--pty` is cross-platform on the client side.**
`cli_app` never forks a pty itself — `--pty` just wraps this process's own
inherited `fd 0`/`fd 1` in `stdio_client_transport`, which is identical on
every platform. Only the server-side `pty_process` (`src/pty`), which
actually forks a terminal, is Linux-only; it throws
`std::runtime_error` on Windows.

**`--pty` mode can't read `fd 0` via `std::cin`.**
When launched inside a `bison_server --pty` session, `fd 0`/`fd 1` are the
pty slave the spawned shell also uses, and `stdio_client_transport`'s
background reader owns `fd 0` (in non-blocking mode, scanning for `BISON:`
frames) for the whole session. A concurrent `std::cin` read on the same fd
loses that race and fails immediately, ending the REPL before the operator
can type anything. `client_app::read_console_line()` (declared in
`client_app.hpp`, since this is a `--pty`-transport concern, not a REPL
concern) instead reads `std::cin` only in socket/pipe mode; in `--pty` mode
it drains a line queue fed by the transport's own passthrough callback,
which already receives every non-`BISON:` byte in arrival order. See
`src/pty/DESIGN.md` for the matching server-side and framing-corruption
notes.

**`proxy::dynamic` stored directly in `std::unordered_map`.**
`proxy::dynamic` is move-only.  Insertion uses `try_emplace`; removal uses
`extract()` to take ownership before passing to `client.destroy()`.

**Name dictionary caching lives in `cli_app`, not in `rmi::client`.**
`rmi::client` is a general-purpose RMI session object with no concept of
display names or human-readable output.  Storing a `key_name_map` there
would pollute the client API with CLI-specific concerns.  Instead, `cli_app`
fetches the dictionary once after connect and manages the cache locally.
Subclasses (e.g. "wish") may call `get_dictionary()` themselves and apply
`merge_dictionary()` to their own map.

**`OP_HELP` auto-generates its listing from the registry.**
The server builds the help text by walking the same registry used by
`OP_DESCRIBE`.  No manual documentation string is required at the server
level; `server_app` subclasses may override `server_description()` to prepend
a custom intro paragraph, but the class/field/method listing is always
generated automatically.  Only items with a `DisplayName` attribute appear
in the listing, keeping the output readable even when hashes are involved.

## 8. Constraints and Invariants

- All proxies in the variable table must be destroyed (via `client.destroy()`)
  before `client.disconnect()` is called. `on_session()` drains the table on
  every exit path before returning.
- The REPL reads from `std::cin` (socket/pipe transports) or a passthrough
  line queue (`--pty` transport) via `client_app::read_console_line()`;
  redirecting stdin enables non-interactive scripting in the former case.
- `from_json()` is used for all JSON → dynamic conversions; it throws on
  malformed input. The REPL catches these exceptions and prints to stderr.
- `--timeout MS` applies uniformly to all blocking `.get()` calls via
  `std::future::wait_for`.

## 9. Integration Boundaries

Depends on:

- `src/rmi/client/client` — `rmi::client` for all RPC operations
  (`get_dictionary()`, `get_help()`, `describe()`, etc.).
- `src/rmi/proxy` — `proxy::dynamic` for object handles.
- `src/rmi/transport/socket_transport` — default TCP transport.
- `src/rmi/transport/named_pipe_transport` — `--pipe` transport.
- `src/rmi/transport/stdio_transport` — `--pty` transport (cross-platform;
  wraps this process's own stdio, does not spawn a pty).
- `src/bison/bison.hpp` — `bison::dynamic`, `bison::key_t`.
- `src/bison/bison_object.hpp` — `bdg::bison::extensions::from_json()`.
- `src/rmi/shared/constants.hpp` — well-known field-name and operation constants.
- `extern/json` — `nlohmann::json` used in `dynamic_to_json` and `unquote`.

Depended on by:

- Nothing in `src/rmi` or `src/bison` depends on `src/app/cli`.
- The `bison-cli` binary produced from `main.cpp`.
- Downstream projects (e.g. "wish") that subclass `cli_app`.
