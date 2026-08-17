# Running Examples

All examples below are the native C++ ones, under `examples/`, except "Android Example". Build the project first (see [building.md](building.md)). New to the library? [tutorial.md](tutorial.md) walks through the same material concept-by-concept. Looking for the Python or C# examples instead? See [bindings.md](bindings.md).

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

`--transport=tls` works the same way, with certificate flags added. A
ready-to-use set of throwaway dev certificates already lives in
[examples/certs/](../examples/certs/) (self-signed CA + server cert with a
`localhost`/`127.0.0.1` SAN, plus a client CA + client cert for mutual TLS)
-- see [docs/tls.md](tls.md) if you need to regenerate them or want the full
flag reference:

```bash
# Server-only TLS
./build/examples/rmi_server_example --transport=tls \
    --cert_file=examples/certs/server-cert.pem --key_file=examples/certs/server-key.pem
./build/examples/rmi_client_example --transport=tls \
    --ca_file=examples/certs/ca-cert.pem
```

```bash
# Mutual TLS -- server also verifies the client's certificate
./build/examples/rmi_server_example --transport=tls \
    --cert_file=examples/certs/server-cert.pem --key_file=examples/certs/server-key.pem \
    --client_auth=required --ca_file=examples/certs/client-ca-cert.pem
./build/examples/rmi_client_example --transport=tls \
    --ca_file=examples/certs/ca-cert.pem \
    --cert_file=examples/certs/client-cert.pem --key_file=examples/certs/client-key.pem
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

Either side (or both) can be TLS-secured with `--downstream_transport=tls`/
`--upstream_transport=tls` and the matching `downstream_`/`upstream_`-prefixed
certificate flags -- see [docs/tls.md](tls.md)'s "Bridge" section for the full
flag reference and a mutual-TLS example.

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

## Android Example (emulator)

`bindings/android/examples/BisonExample` is a small Kotlin app exercising the
Android binding (`bindings/android/` -- see [bindings.md](bindings.md#android-java--kotlin-bindingsandroid))
on-device: the `Dynamic` quick start, a locally registered method, and an
in-process RMI round trip (`rmi::standalone`), each step logged to the
screen. Requires the Android SDK/NDK and an emulator or device (Android
Studio's SDK Manager is the easiest way to get both; see
[building.md](building.md#building-for-android) for the NDK requirement).

```bash
cd bindings/android

# Build and install on a running emulator/device:
./gradlew :examples:BisonExample:installDebug
adb shell am start -n com.bdg.bison.example/.MainActivity
```

Or open `bindings/android/` directly in Android Studio and run the
`BisonExample` configuration on an `x86_64` AVD (create one via **Tools >
Device Manager** if needed -- an x86_64 system image gives the emulator
native-speed CPU emulation, unlike `arm64-v8a` on an x86_64 host).

The app runs its demo automatically on launch; a successful run ends with
"All steps completed successfully." There is no re-run button: the demo's
RMI step registers a class into bison's process-lifetime class registry
(no unregister call exists), so a second run in the same process would
throw "attempted to add a duplicate class or method" -- relaunch via
`adb shell am force-stop com.bdg.bison.example && adb shell am start -n
com.bdg.bison.example/.MainActivity` instead. To validate just the binding
itself (no UI) the same way CI would, run its instrumented test suite
instead:

```bash
./gradlew :bison-lib:connectedAndroidTest
# Report: bindings/android/bison-lib/build/reports/androidTests/connected/index.html
```

## Performance Benchmark

```bash
# Build in Release for meaningful timings:
cmake --build build --config Release --target bison_performance

./build/examples/bison_performance 100000
./build/examples/bison_performance --iterations=100000 --samples=7 --warmup=2 --format=markdown
```

Supported `--format` values: `table`, `csv`, `markdown`.

The benchmark compares plain C++ struct, `bison::dynamic`, and `nlohmann::json` across: create/destroy, field set/get, method calls, serialize, deserialize. See [performance.md](performance.md) for architecture details.
