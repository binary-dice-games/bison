# Running Examples

All examples are under `examples/`. Build the project first (see [building.md](building.md)).

## C++ Core Example (`bison_examples`)

```bash
cmake --build build --config Debug --target bison_examples
./build/examples/bison_examples
```

## RMI Socket Examples

Two processes: server and client communicating over TCP. `rmi_server_example`
and `rmi_client_example` build the transport manually with `rmi.hpp` (unlike
`calc-server`/`bison-cli`, which use the `server_app`/`client_app` scaffold),
but accept the same `--transport`/`--host`/`--port`/`--name`/`--cmd`/
`--debugger` flags, so usage is consistent across the project.

```bash
cmake --build build --config Debug --target rmi_server_example rmi_client_example
# Start server first:
./build/examples/rmi_server_example
# Then in another terminal:
./build/examples/rmi_client_example
# Optional flags: --host=HOST --port=PORT
```

## RMI Term Hop Mode (OSC-99, Linux/MSYS2 and Windows)

`--transport=term` is an interactive terminal hop, spawning a real shell
(or `--cmd`) via `src/term/terminal.hpp`, but framing RMI traffic as OSC-99
escape sequences instead of literal `BISON<...>` text (see [FORMAT.md](../FORMAT.md) §5.3).
This is designed to survive being relayed through a Windows ConPTY, which 
tends to mangle literal `BISON<...>` text embedded in the stream but reliably
shepherds OSC sequences through untouched.

**Server side:**

```bash
./build/src/srv/calc/calc-server --transport=term
```

Optionally pass `--cmd` to spawn something other than the default shell
(`$SHELL` on Linux/MSYS2, `cmd.exe` on native Windows).

**Client side**, launched from inside the spawned terminal exactly like
`--transport=term`:

```bash
./bison-cli --transport=term
```

The client does not spawn anything here either — it wraps its own
inherited stdin/stdout in OSC-99 framing. Non-OSC-99 bytes (shell prompts,
operator-typed output) pass straight through, so the session stays fully
interactive.

## Performance Benchmark

```bash
# Build in Release for meaningful timings:
cmake --build build --config Release --target bison_performance

./build/examples/bison_performance 100000
./build/examples/bison_performance --iterations=100000 --samples=7 --warmup=2 --format=markdown
```

Supported `--format` values: `table`, `csv`, `markdown`.

The benchmark compares plain C++ struct, `bison::dynamic`, and `nlohmann::json` across: create/destroy, field set/get, method calls, serialize, deserialize. See [performance.md](performance.md) for architecture details.
