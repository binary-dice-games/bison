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

The C-ABI equivalents (`rmi_abi_server_example` / `rmi_abi_client_example`,
built on `rmi_c.h` only) accept the same `--transport`/`--host`/`--port`/
`--name` flag names. `rmi_c.h` exposes `rmi_client_pty_create()` /
`rmi_server_pty_create()` and `rmi_client_console_create()` /
`rmi_server_console_create()` alongside the tcp/pipe constructors, and
`rmi_abi_client_example` wires up `--transport=pty|console` for the client
side; `rmi_abi_server_example` sticks to `tcp`/`pipe` since detecting "the
spawned shell/subprocess exited" to know when to call `rmi_server_stop()`
needs a wait-for-child primitive this minimal example doesn't build (see
`rmi_server_example`'s pty/console branches for the C++ equivalent, which use
`pty_process`/`console_process` directly).

## RMI PTY / Stdio Hop Mode

`server_app`/`client_app` (the base classes behind `calc-server` and
`bison-cli`) support `--transport=pty`, which tunnels RMI traffic as base64
`BISON<...>` frames over an interactive terminal — useful when the only path
to a remote host is a terminal program (`ssh`, `adb shell`, etc.), not a
socket or named pipe. `rmi_server_example`/`rmi_client_example` support the
same flag directly against `rmi.hpp`. See [FORMAT.md](../FORMAT.md) for the
wire framing and `src/pty/DESIGN.md` for the architecture.

**Server side** (Linux / WSL only — forks a real pty and your `$SHELL`):

```bash
./build-linux/src/srv/calc/calc-server --transport=pty
```

This drops you into an ordinary, fully interactive shell. From inside it,
hop to wherever the RMI session needs to run (e.g. `ssh host`), then launch
a bison client there as a plain child process of that shell:

```bash
./bison-cli --transport=pty
```

The client's `--transport=pty` does not spawn anything — it just wraps its own
already-inherited stdin/stdout in the same `BISON<...>` framing, so it
works identically on Linux and MSYS2 with no subprocess involved.
Anything the client doesn't recognize as a `BISON<...>` frame (shell
prompts, command output typed by the operator) passes straight through to
the terminal byte-for-byte, so the session stays fully interactive.

## RMI Console (Non-Interactive Subprocess) Hop Mode

`--transport=console` is `--transport=pty`'s non-interactive sibling: the
server spawns a subprocess given by `--cmd` (via libuv's `uv_spawn`, not a
pty) and pipes its stdin/stdout through the same `BISON<...>` framing —
no terminal, no keystroke forwarding. This is for bridging a server and a
client purely over stdio, e.g. across an SSH hop with no interactive
terminal involved:

```bash
./build/src/srv/calc/calc-server --transport=console \
  --cmd="ssh myuser@example.com ./bison-cli --transport=console"
```

The server (`--cmd`) side spawns the subprocess and shuts down once it
exits. The client side never takes a `--cmd` — like `--transport=pty`, it
just wraps its own inherited fd 0/1 in the `BISON<...>` framing, so
`./bison-cli --transport=console` on the remote host works whether it was
launched by SSH, another shell, or directly. See `src/console/console_process.hpp`
for the `uv_spawn`-based process wrapper.

## RMI Term Hop Mode (OSC-99, Linux/MSYS2 and Windows)

`--transport=term` is `--transport=pty`'s sibling: also an interactive
terminal hop, spawning a real shell (or `--cmd`) via `src/term/terminal.hpp`,
but framing RMI traffic as OSC-99 escape sequences instead of literal
`BISON<...>` text (see [FORMAT.md](../FORMAT.md) §5.3). This is designed to
survive being relayed through a Windows ConPTY, which tends to mangle
literal `BISON<...>` text embedded in the stream but reliably shepherds OSC
sequences through untouched. Unlike `--transport=pty` (Linux/MSYS2 only,
`forkpty()`), `--transport=term`'s server side also builds on native
Windows via ConPTY — see `src/term/terminal_win.cpp`.

**Server side:**

```bash
./build/src/srv/calc/calc-server --transport=term
```

Optionally pass `--cmd` to spawn something other than the default shell
(`$SHELL` on Linux/MSYS2, `cmd.exe` on native Windows).

**Client side**, launched from inside the spawned terminal exactly like
`--transport=pty`:

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
