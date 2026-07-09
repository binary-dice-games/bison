# Running Examples

All examples are under `examples/`. Build the project first (see [building.md](building.md)).

## C++ Core Example (`bison_examples`)

```bash
cmake --build build --config Debug --target bison_examples
./build/examples/bison_examples
```

## RMI Socket Examples

Two processes: server and client communicating over TCP. `rmi_server_example`
and `rmi_client_example` are built directly on the `server_app`/`client_app`
scaffold (the same base classes used by `calc-server`/`bison-cli`), and accept
the same `--transport`/`--host`/`--port`/`--name`/`--cmd`/`--debugger` flags,
so usage is consistent across the project.

```bash
cmake --build build --config Debug --target rmi_server_example rmi_client_example
# Start server first:
./build/examples/rmi_server_example
# Then in another terminal:
./build/examples/rmi_client_example
# Optional flags: --host=HOST --port=PORT
```

## RMI Standalone Example

`rmi_standalone_example` demonstrates `rmi::standalone` (in-process,
transport-free RMI) via the `standalone_app` scaffold -- the native-C++
counterpart to `rmi_abi_standalone_example`, which exercises the same
`rmi_standalone_create()` entry point through the C ABI.

```bash
cmake --build build --config Debug --target rmi_standalone_example
./build/examples/rmi_standalone_example
```

## RMI Bridge Example

`rmi_bridge_example` demonstrates `rmi::bridge` via the `bridge_app` scaffold:
it accepts downstream client connections and transparently relays every
operation to one upstream server. Run all three examples together to see a
client talk through the bridge exactly as if it were talking directly to the
server:

```bash
cmake --build build --config Debug --target rmi_server_example rmi_bridge_example rmi_client_example

# 1. Real Calculator server on port 7070:
./build/examples/rmi_server_example --transport=tcp --port=7070

# 2. Bridge: downstream TCP on 7071, relays to upstream TCP on 7070
#    (--upstream_transport defaults to term, so pass tcp explicitly here):
./build/examples/rmi_bridge_example --downstream_port=7071 --upstream_transport=tcp --upstream_port=7070

# 3. Client talks to the bridge instead of the real server:
./build/examples/rmi_client_example --port=7071
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
