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

## RMI Stdio / PTY Examples (Linux / WSL only)

```bash
cmake --build build-linux --target rmi_stdio_server_example rmi_stdio_client_example
```

**PTY mode** — interactive shell hop (bash, ssh, adb):

```bash
./build-linux/examples/rmi_stdio_client_example --pty bash
# Inside the spawned shell, start the server:
./build-linux/examples/rmi_stdio_server_example
```

**Pipe mode** — direct subprocess:

```bash
./build-linux/examples/rmi_stdio_client_example --pipe ./build-linux/examples/rmi_stdio_server_example
# Or over SSH:
./build-linux/examples/rmi_stdio_client_example --pipe ssh user@host /path/to/rmi_stdio_server_example
```

Expected output:
```
[Client] subprocess started. Waiting for HELLO...
[Server] stdio transport listening.
[Client] HELLO received. RMI channel connected.
[Client] instantiated Calculator, id=...
[Client] add(10, 3) = 13
[Client] divide(21, 7) = 3
[Client] done.
[Server] stopped.
```

### PTY library API

The PTY flow is available as reusable base classes (Linux only):

```cpp
// Server: owns a bash subprocess via forkpty; serves objects over DCS-framed bison
class MyServerApp : public bdg::bison::app::pty_server_app {
 protected:
    void register_classes() override { /* register server-side classes */ }
    void on_client_connected() const override { /* optional hook */ }
    void on_session_ended() const override { /* optional hook */ }
};

// Client: runs inside the bash session (e.g. after ssh into the server host)
class MyClientApp : public bdg::bison::app::pty_client_app {
 protected:
    int on_session(bdg::bison::rmi::client& c) override {
        return 0;
    }
};

int main(int argc, char** argv) {
    MyServerApp app;  // or MyClientApp
    return app.run(argc, argv);
}
```

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
