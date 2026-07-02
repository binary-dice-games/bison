# Running Examples

All examples are under `examples/`. Build the project first (see [building.md](building.md)).

## C++ Core Example (`bison_examples`)

```bash
cmake --build build --config Debug --target bison_examples
# Windows:
.\build\examples\Debug\bison_examples.exe
```

## RMI Socket Examples

Two processes: server and client communicating over TCP.

```bash
cmake --build build --config Debug --target rmi_server_example rmi_client_example
# Start server first:
.\build\examples\Debug\rmi_server_example.exe
# Then in another terminal:
.\build\examples\Debug\rmi_client_example.exe
# Optional args: [host] [port]
```

## RMI PTY / Stdio Hop Mode

`server_app`/`client_app` (the base classes behind `calc-server` and
`bison-cli`) support a `--pty` flag that tunnels RMI traffic as base64
`BISON:` lines over an interactive terminal — useful when the only path to
a remote host is a terminal program (`ssh`, `adb shell`, etc.), not a
socket or named pipe. See [FORMAT.md](../FORMAT.md) for the wire framing
and `src/pty/DESIGN.md` for the architecture.

**Server side** (Linux / WSL only — forks a real pty and your `$SHELL`):

```bash
./build-linux/src/srv/calc/calc-server --pty
```

This drops you into an ordinary, fully interactive shell. From inside it,
hop to wherever the RMI session needs to run (e.g. `ssh host`), then launch
a bison client there as a plain child process of that shell:

```bash
./bison-cli --pty
```

The client's `--pty` does not spawn anything — it just wraps its own
already-inherited stdin/stdout in the same `BISON:` line framing, so it
works identically on Windows and Linux with no subprocess involved.
Anything the client doesn't recognize as a `BISON:` line (shell prompts,
command output typed by the operator) passes straight through to the
terminal byte-for-byte, so the session stays fully interactive.

## Performance Benchmark

```bash
# Build in Release for meaningful timings:
cmake --build build --config Release --target bison_performance

# Windows:
.\build\examples\Release\bison_performance.exe 100000
.\build\examples\Release\bison_performance.exe --iterations=100000 --samples=7 --warmup=2 --format=markdown
```

Supported `--format` values: `table`, `csv`, `markdown`.

The benchmark compares plain C++ struct, `bison::dynamic`, and `nlohmann::json` across: create/destroy, field set/get, method calls, serialize, deserialize. See [performance.md](performance.md) for architecture details.
