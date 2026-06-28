# PTY Abstraction Layer Design

## 1. Purpose and Scope

`src/rm/pty` provides a platform-independent C++ abstraction for pseudoterminals.
It exposes two extensible base classes — `pty_server` and `pty_client` — that
handle PTY creation, subprocess lifecycle, and DCS escape-sequence interception
transparently on Linux (`forkpty`) and Windows (ConPTY).

Subclasses override virtual hooks to process intercepted escape sequences and to
inject their own. The bison RMI layer uses this to transmit bison envelopes
over the stdin/stdout of an interactive shell via DCS framing (see
`src/rmi/transport/dcs_framing.hpp`), while normal shell I/O passes through
untouched.

This directory owns:

- `pty_server.hpp` — platform-independent `pty_server` base class declaration.
- `pty_client.hpp` — platform-independent `pty_client` base class declaration.
- `pty_server_linux.cpp` — Linux implementation (`forkpty`, `termios`, libuv
  `uv_pipe_open`).
- `pty_server_win.cpp` — Windows implementation (ConPTY, `CreatePseudoConsole`,
  libuv `uv_pipe_open`).
- `pty_client.cpp` — single cross-platform implementation (libuv `uv_tty_t` /
  `uv_pipe_t` via `uv_guess_handle`).

This directory does **not** own:

- RMI transport adapters — those live in `src/rmi/transport/pty_*_transport`.
- DCS framing helpers — those live in `src/rmi/transport/dcs_framing.hpp`.
- Application scaffolds (`pty_server_app`, `pty_client_app`) — those live in
  `src/app/pty`.

## 2. Why a Real PTY Is Required

**The fundamental constraint:** shells and interactive programs (bash, cmd,
PowerShell, less, vim, etc.) call `isatty(stdin)` to detect whether they are
attached to a terminal. If the answer is false — which is always the case when
the process is connected via ordinary pipes — they permanently disable:

- Readline editing (arrow keys, history, Ctrl-A/E, completion)
- Colour output and ANSI escape sequences
- Job control (`Ctrl-C`, `Ctrl-Z`, `fg`, `bg`)
- Prompts on stdin (the `PS1` prompt is suppressed entirely in some shells)
- Interactive mode for interpreters (Python, Node, etc.)

A previous attempt used `uv_spawn()` with `UV_CREATE_PIPE` stdio — giving the
child a pair of anonymous pipes. Although DCS framing worked correctly on those
pipes, bash ran in non-interactive, non-TTY mode and the console was unusable.

**The fix:** `pty_server` must create the subprocess inside a **real PTY** using
`forkpty()` (Linux) or `CreatePseudoConsole()` (Windows). Both calls allocate a
master/slave PTY pair and spawn the child with its stdin/stdout/stderr connected
to the PTY slave. From the child's perspective, `isatty(0)` returns true and the
shell behaves exactly as if a human were sitting at a terminal.

libuv's `uv_spawn()` cannot be used here because it does not support attaching a
child process to a PTY. It is used elsewhere (anonymous pipe worker transports)
where interactivity is not required.

## 3. Architecture Decision: Composition with Callbacks

Three architectures were considered for connecting the PTY layer to the RMI layer.

### Option A — Transport inherits PTY base class (rejected)

```
pty_server_transport : pty_server, server_transport_iface
  override on_escape_sequence(body) → push to inbox
```

This was rejected because the IS-A relationship is wrong. A transport is not a
PTY; it owns one. Inheritance would force callers who want `pty_server` for
non-RMI uses to subclass it, and it would couple the PTY layer's virtual
dispatch to the RMI layer's internal queue management.

### Option B — Memory transport + PTY bridge (rejected)

```
rmi::server  ←——  memory_server_transport  ←——  pty_bridge  ←— DCS —→  PTY master
rmi::client  ←——  memory_client_transport  ←——  pty_bridge  ←— DCS —→  stdin/stdout
```

Rejected: the `pty_bridge` adapter would need to implement both the memory
transport consumer interface and the DCS producer interface — as much code as
implementing the transport interface directly, plus an extra mutex-protected
queue copy on every frame.

### Option C — Transport contains PTY as a member, callbacks (chosen)

```
pty_server_transport : server_transport_iface
  pty_server pty_;          ← HAS-A, not IS-A
  pty_.set_callbacks(...)   ← set in constructor; no subclassing

rmi::server  ←——→  pty_server_transport  ←— DCS —→  PTY master
                    │                                    │
                    └── pty_server (member)          PTY slave
                                                         │
rmi::client  ←——→  pty_client_transport  ←— DCS —→  (stdin/stdout
                    │                                  via SSH or
                    └── pty_client (member)            ConPTY)
```

`pty_server_transport` stores a `pty_server` value member and sets its callbacks
in the transport's constructor. When the PTY fires `on_escape_sequence`, the
callback (a lambda capturing the transport's `impl*`) decodes the BISON_RMI/1
body and pushes the frame into the inbox queue. When the transport's `send()` is
called, it calls `pty_.inject_escape_sequence(body)`.

This keeps the two layers independent: `pty_server` has no knowledge of RMI;
`pty_server_transport` has no knowledge of PTY internals beyond the public API.

`memory_server_transport` / `memory_client_transport` remain the correct choice
for in-process unit tests that bypass the PTY entirely.

## 4. Design Goals

1. **Real PTY, real terminal** — the spawned process has `isatty(0) == true`, so
   readline, colour codes, job control, and interactive shells work as if the
   user were at a physical terminal.
2. **Bidirectional escape-sequence channel** — both server and client can send
   and receive DCS frames at any time. DCS frames travel invisibly in both
   directions alongside normal terminal I/O; the user never sees raw escape
   sequences.
3. **Platform-independent interface** — `pty_server` and `pty_client` have
   identical public and virtual APIs on Linux and Windows. Platform differences
   are fully contained in `_linux.cpp` and `_win.cpp`.
4. **libuv for I/O after PTY creation** — once `forkpty` / `CreatePseudoConsole`
   returns a fd or pipe handle, libuv streams (`uv_pipe_open`) drive all
   subsequent I/O, write queuing, and shutdown signalling. Only the PTY
   allocation step itself requires platform-specific OS calls.
5. **Extensibility** — callers set only the callbacks they need; all others have
   a sensible default.

## 5. Deployment Scenarios

### 5.1 SSH relay (primary use case)

The canonical use case for the bison PTY transport:

```
Local machine                          Remote machine (via SSH)
──────────────────────────             ──────────────────────────────
pty_server_transport                   pty_client_transport
  └── pty_server (bash in PTY) ◄─ SSH channel ─► (stdin/stdout = PTY slave)
         │ DCS frames                                    │ DCS frames
         ▼                                               ▼
     rmi::server                                     rmi::client
```

1. `pty_server` forks bash in a real PTY. The user can type shell commands
   normally; the `on_output` callback relays non-DCS bytes to the local terminal
   so the user sees the shell.
2. The user SSH-connects to the remote machine from within that bash session.
   The remote shell's stdin/stdout flow through the SSH channel and back to the
   local PTY slave.
3. On the remote machine, the user starts a bison `pty_client` binary. Its
   stdin/stdout IS the PTY slave (presented as a TTY by SSH). `uv_guess_handle`
   selects `uv_tty_t` and raw mode is set.
4. **Server → client:** DCS frames written by `pty_server::inject_escape_sequence`
   travel PTY master → PTY slave → SSH → remote stdin. The remote `pty_client`
   extracts them and delivers them to the `on_escape_sequence` callback.
5. **Client → server:** DCS frames written by `pty_client::send_escape_sequence`
   travel remote stdout → SSH → PTY slave → PTY master. The local `pty_server`
   extracts them and delivers them to the `on_escape_sequence` callback.
   Both directions are active simultaneously; neither side needs to wait for the
   other before sending.

### 5.2 Direct subprocess (testing / controlled launch)

For integration tests or controlled environments where the server directly
spawns the client:

```
pty_server_transport
  └── pty_server (spawns pty_client_binary in PTY via forkpty/ConPTY)
                         │
                  pty_client_binary
                    └── pty_client_transport
                         (stdin = PTY slave → uv_tty_t, raw mode)
```

The server uses `forkpty` / ConPTY to spawn the `pty_client` binary, giving it
a real PTY slave as stdin. `uv_guess_handle(0)` on the client side returns
`UV_TTY` and the client opens a `uv_tty_t` in raw mode — DCS frames pass through
without any buffering or translation.

**Do not use `uv_spawn` with pipes for this.** Pipe-spawned clients have
`isatty(0) == false`; although DCS framing would still work for the bison channel,
the client process itself would run in non-TTY mode which is the same
interactivity problem described in §2.

### 5.3 stdin/stdout mode auto-detection

`pty_client` uses `uv_guess_handle(0)` to select the right libuv handle type
automatically. No caller configuration is required:

| `stdin` context | `uv_guess_handle` result | libuv handle | Mode |
|---|---|---|---|
| Real TTY (SSH session, local terminal) | `UV_TTY` | `uv_tty_t` | `UV_TTY_MODE_RAW` |
| PTY slave (spawned inside pty_server PTY) | `UV_TTY` | `uv_tty_t` | `UV_TTY_MODE_RAW` |
| Anonymous pipe (test harness, non-interactive) | `UV_NAMED_PIPE` | `uv_pipe_t` | n/a |

In all three cases DCS frames are transmitted identically; only the low-level
handle type differs. The `on_escape_sequence` override receives the same bytes
regardless.

## 6. Key Abstractions

### 6.1 `pty_server`

A concrete, non-abstract utility class. Owns the PTY master and child subprocess.
The PTY master fd/handle is a **bidirectional** channel: bytes written to it
reach the subprocess's stdin; bytes produced by the subprocess's stdout come back
out of it. `pty_server` drives three I/O paths over this channel:

- **Terminal relay (receive)** — non-DCS bytes arriving from the subprocess are
  delivered to the `on_output` callback. The default writes them to the caller's
  `stdout` so the user sees normal shell output.
- **Escape-sequence receive** — DCS frames arriving from the subprocess (i.e.
  sent by `pty_client::send_escape_sequence` on the other end) are extracted by
  `dcs_byte_parser` and delivered to the `on_escape_sequence` callback.
- **Escape-sequence send** — the caller writes DCS frames toward the subprocess
  (i.e. toward `pty_client`) by calling `inject_escape_sequence`. These bytes
  are written to the PTY master and appear on the subprocess's stdin.

Both the send and receive escape-sequence paths are active simultaneously once
`start()` is called. Callbacks are grouped in a `callbacks` struct and passed at
construction. No subclassing is required or expected.

```cpp
namespace bdg::bison::rm::pty {

class pty_server {
 public:
  using output_fn   = std::function<void(const uint8_t* data, size_t n)>;
  using escape_fn   = std::function<void(const std::string& body)>;
  using exit_fn     = std::function<void(int64_t exit_status, int term_signal)>;

  struct callbacks {
    output_fn on_output;           ///< non-DCS bytes from subprocess (default: write to stdout)
    escape_fn on_escape_sequence;  ///< complete DCS body from subprocess (default: no-op)
    exit_fn   on_exit;             ///< subprocess exited (default: no-op)
  };

  explicit pty_server(callbacks cbs = {});
  ~pty_server();

  pty_server(pty_server&&) noexcept;
  pty_server& operator=(pty_server&&) noexcept;

  pty_server(const pty_server&)            = delete;
  pty_server& operator=(const pty_server&) = delete;

  /**
   * @brief Spawn @p command inside a real PTY and start the event loop.
   *
   * Linux: calls forkpty() so the child has isatty(0)==true.
   * Windows: calls CreatePseudoConsole() + CreateProcess().
   * Wraps the resulting fd/HANDLE in a libuv uv_pipe_t and starts the
   * background loop thread.
   *
   * @throws std::runtime_error on spawn failure.
   */
  void start(const std::string& command);

  /** @brief Signal the subprocess and shut down the event loop. */
  void stop();

  /** @brief Return true while the subprocess is alive. */
  bool is_running() const;

  /**
   * @brief Write @p data to the PTY master (user keystrokes or injected input).
   * Thread-safe; enqueued via uv_async_t and written on the loop thread.
   */
  void send_input(const uint8_t* data, size_t n);

  /**
   * @brief Emit a DCS escape sequence into the PTY master.
   *
   * Wraps @p body as `ESC P <body> ESC \` and enqueues it. The subprocess
   * stdin receives the raw DCS bytes; pty_client's dcs_byte_parser will
   * extract them and fire the on_escape_sequence callback on the other end.
   * Thread-safe.
   */
  void inject_escape_sequence(const std::string& body);

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace bdg::bison::rm::pty
```

### 6.2 `pty_client`

A concrete, non-abstract utility class. The process's own `stdin` / `stdout`
form a **bidirectional** channel toward `pty_server`: bytes written to `stdout`
flow back through the PTY to the server's `on_escape_sequence` callback; bytes
produced by the server's `inject_escape_sequence` arrive on `stdin`.
`pty_client` drives three I/O paths:

- **Non-DCS receive** — non-DCS bytes arriving on `stdin` are delivered to the
  `on_stdin_data` callback (e.g. shell prompts the user typed earlier, echoed
  output).
- **Escape-sequence receive** — DCS frames arriving on `stdin` (sent by
  `pty_server::inject_escape_sequence`) are extracted by `dcs_byte_parser` and
  delivered to the `on_escape_sequence` callback.
- **Escape-sequence send** — the caller writes DCS frames toward the server by
  calling `send_escape_sequence`. These bytes are written to `stdout` and travel
  through the PTY to the server's `on_escape_sequence` callback.

Both the send and receive escape-sequence paths are active simultaneously once
`start()` is called. `uv_guess_handle` selects the right libuv handle type
automatically (§5.3). Callbacks are set at construction; no subclassing required.

```cpp
namespace bdg::bison::rm::pty {

class pty_client {
 public:
  using stdin_fn  = std::function<void(const uint8_t* data, size_t n)>;
  using escape_fn = std::function<void(const std::string& body)>;
  using closed_fn = std::function<void()>;

  struct callbacks {
    stdin_fn  on_stdin_data;       ///< non-DCS bytes from stdin (default: no-op)
    escape_fn on_escape_sequence;  ///< complete DCS body from stdin (default: no-op)
    closed_fn on_closed;           ///< stdin closed (default: no-op)
  };

  explicit pty_client(callbacks cbs = {});
  ~pty_client();

  pty_client(pty_client&&) noexcept;
  pty_client& operator=(pty_client&&) noexcept;

  pty_client(const pty_client&)            = delete;
  pty_client& operator=(const pty_client&) = delete;

  /**
   * @brief Start reading stdin and the libuv event loop.
   *
   * @param set_raw_mode  When true and stdin is a TTY, switch to raw mode so
   *                      DCS frames pass through without buffering or echo.
   *                      Mode is restored on stop().
   */
  void start(bool set_raw_mode = true);

  /** @brief Stop the event loop and restore terminal mode if changed. */
  void stop();

  /**
   * @brief Emit a DCS escape sequence on stdout.
   * Wraps @p body as `ESC P <body> ESC \` and writes it on the loop thread.
   * Thread-safe.
   */
  void send_escape_sequence(const std::string& body);

  /**
   * @brief Write raw bytes to stdout (pass-through terminal output).
   * Thread-safe; enqueued via uv_async_t.
   */
  void send_output(const uint8_t* data, size_t n);

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace bdg::bison::rm::pty
```

## 7. Platform Implementation Strategy

### 7.1 PTY creation — unavoidably platform-specific

PTY allocation requires OS-specific calls. Per the project coding style this is
split into two source files; no `#ifdef` appears in the shared header:

| File | Platform | Key OS calls |
|---|---|---|
| `pty_server_linux.cpp` | Linux | `forkpty()`, `tcgetattr()` / `tcsetattr()`, `waitpid()` |
| `pty_server_win.cpp` | Windows | `CreatePseudoConsole()`, `CreateProcess()`, `ClosePseudoConsole()` |

### 7.2 libuv for everything after PTY creation

Once `forkpty` returns the master fd, or `CreatePseudoConsole` yields a pair of
pipe handles, the platform-specific code:

1. Opens a libuv `uv_pipe_t` around the fd/HANDLE via `uv_pipe_open()`.
2. Calls `uv_read_start()` and feeds each buffer through `dcs_byte_parser`.
3. Writes are queued via `uv_async_t` → `uv_write()` on the loop thread.
4. Shutdown signals the child (SIGTERM / `TerminateProcess`), then calls
   `uv_async_send(&stop_async)` to close all handles and let `uv_run` return.

`pty_client.cpp` is a single cross-platform file because libuv's
`uv_guess_handle(0)` transparently selects `uv_tty_t` or `uv_pipe_t`, and
`uv_tty_set_mode(UV_TTY_MODE_RAW)` works on both platforms.

### 7.3 Platform mapping

| Concept | Linux | Windows |
|---|---|---|
| PTY creation | `forkpty()` → `int master_fd` | `CreatePseudoConsole()` → `HANDLE hRead, hWrite` |
| libuv wrapping | `uv_pipe_open(master_fd)` | `uv_pipe_open(hRead)` + `uv_pipe_open(hWrite)` |
| Process wait | `waitpid()` or `uv_process_t exit_cb` | `WaitForSingleObject(hProcess)` |
| Raw mode (client TTY) | `uv_tty_set_mode(UV_TTY_MODE_RAW)` | Same libuv call |
| Terminal size | `ioctl(TIOCSWINSZ)` | `ResizePseudoConsole()` |

**Windows notes:**

- ConPTY requires Windows 10 1809+ (SDK 17763, `_WIN32_WINNT=0x0A00`). Set
  per-source in CMake to avoid raising the global `_WIN32_WINNT` used by ASIO.
- `CreatePseudoConsole` takes `hInRead` and `hOutWrite` (child-facing pipe ends).
  The server-facing ends `hInWrite` (write to child) and `hOutRead` (read from
  child) are wrapped by libuv. The child-facing ends must be closed immediately
  after `CreatePseudoConsole` returns — ConPTY takes ownership of them.

## 8. Escape-Sequence Protocol

DCS frames carry the out-of-band bison channel. The encoding follows the format
already defined in `src/rmi/transport/dcs_framing.hpp`:

```
ESC P <body> ESC \
```

`dcs_byte_parser` (from `dcs_framing.hpp`) handles the byte-level state
machine. Its `on_frame` callback delivers each complete DCS body to
`on_escape_sequence`; its `on_plain` callback delivers non-DCS bytes to
`on_pty_output` (server) or `on_stdin_data` (client).

`pty_server` and `pty_client` are body-format agnostic — the raw body string is
passed as-is. The bison RMI layer uses the `BISON_RMI/1` body format
(`kProtoVersion`, `kTypeData`, `kTypeHello`, `kTypeEnd`) defined in
`dcs_framing.hpp`.

## 9. Data Flow

### Server side — PTY master is bidirectional

```
                        ┌─────────────────────────────────────────────┐
                        │              PTY master fd / HANDLE          │
                        │                                              │
user kbd ──→ send_input()──→[write]                          [read]──→ dcs_byte_parser
                                                                         ├─ non-DCS ──→ on_output()
                                                                         └─ DCS body ──→ on_escape_sequence()

inject_escape_sequence()──→[write]                                       (server sends to client)
                        └─────────────────────────────────────────────┘
                                           │
                                      PTY slave (bash/cmd/ps)
```

### Client side — stdin/stdout are bidirectional

```
                        ┌──────────────────────────────────────────────┐
                        │              stdin          stdout            │
                        │                                               │
[from server]  ──→ stdin──→[read]──→ dcs_byte_parser      [write]──→ stdout ──→ [to server]
                                       ├─ non-DCS ──→ on_stdin_data()
                                       └─ DCS body ──→ on_escape_sequence()

send_escape_sequence() ──────────────────────────────────→[write]──→ stdout
                        └──────────────────────────────────────────────┘
```

### Full SSH relay — both directions active simultaneously

```
Local machine                                   Remote machine
─────────────────────────────────────────────   ─────────────────────────────────
user kbd ──→ send_input()
                 │
                 ▼
          [PTY master write]                     pty_client.send_escape_sequence()
                 │                                         │
                 ▼                                         ▼
          PTY slave (bash) ◄──── SSH channel ────► [stdout write]
                 │                                         │
                 ▼                                         │
          [PTY master read]                                │
         dcs_byte_parser                                   ▼
          ├─ non-DCS ──→ on_output() ──→ local stdout   [stdin read]
          └─ DCS ──→ on_escape_sequence()          dcs_byte_parser
                  (frames from client)              ├─ non-DCS ──→ on_stdin_data()
                                                    └─ DCS ──→ on_escape_sequence()
inject_escape_sequence()                                          (frames from server)
          │
          ▼
    [PTY master write] ──── SSH channel ────► [stdin]
```

Both arrows across the SSH channel carry DCS frames in their respective direction.
Normal terminal I/O (non-DCS bytes) flows left-to-right only (subprocess output
toward the user); it is not echoed back from the client side.

## 10. Thread Model

Both `pty_server` and `pty_client` run a single background **loop thread** that
owns the libuv event loop and all libuv handles:

- All libuv callbacks (`on_read`, `on_write_done`, `on_send`, `on_stop`,
  `on_exit`) execute on the loop thread.
- `send_input`, `inject_escape_sequence`, `send_escape_sequence`, and
  `send_output` enqueue data under a mutex and signal a `uv_async_t`; the write
  is issued on the loop thread.
- `on_pty_output`, `on_escape_sequence`, `on_stdin_data`, and `on_closed` are
  called on the loop thread. Overrides must not block and must not call
  `inject_escape_sequence` / `send_escape_sequence` while holding any mutex that
  the loop thread also acquires (they are safe to call without a held lock).
- `start()` and `stop()` are called from the user thread. `stop()` signals
  `stop_async`, joins the loop thread, and restores terminal mode.

## 11. Integration with bison RMI

`pty_server_transport` and `pty_client_transport` in `src/rmi/transport/`
implement the RMI transport interfaces. They store a `pty_server` / `pty_client`
as a member and wire the callbacks in their own constructor.

```cpp
// Sketch — not the full implementation

class pty_server_transport : public server_transport_iface {
  pty_server pty_;    // HAS-A, not IS-A
  // inbox queue, mutex, condition_variable ...

 public:
  explicit pty_server_transport(std::string command = "bash") {
    pty_ = pty_server({
      .on_output = [](const uint8_t* d, size_t n) {
        // relay non-DCS bytes to stdout so user sees the shell
        ::write(STDOUT_FILENO, d, n);
      },
      .on_escape_sequence = [this](const std::string& body) {
        // decode BISON_RMI/1 DCS body, push frame to inbox
        const auto fields = dcs::parse_fields(body);
        // HELLO → unblock accept(), respond with server HELLO
        // DATA  → dcs::handle_data_frame → push to inbox queue
        // END   → mark session closed
      },
      .on_exit = [this](int64_t, int) { /* close session */ },
    });
  }

  void start(bison::dynamic /*params*/) override { pty_.start(command_); }
  void stop() override { pty_.stop(); }

  // server_connection_iface::send():
  void send(bison::buffer frame) {
    // encode frame as BISON_RMI/1 DCS chunks and inject into PTY
    dcs::emit_data(...);
    pty_.inject_escape_sequence(body);
  }
};

class pty_client_transport : public client_transport_iface {
  pty_client pty_;    // HAS-A

 public:
  pty_client_transport() {
    pty_ = pty_client({
      .on_escape_sequence = [this](const std::string& body) {
        // HELLO → signal hello_received, unblock open()
        // DATA  → dcs::handle_data_frame → push to inbox queue
      },
      .on_closed = [this]() { /* mark closed */ },
    });
  }

  void open(bison::dynamic params) override {
    pty_.start(/*set_raw_mode=*/true);
    // send HELLO frame, then wait for server's HELLO
    pty_.send_escape_sequence(dcs::kProtoVersion + ";type=HELLO");
    // ... wait on recv_cv ...
  }

  void send(bison::buffer frame) override {
    dcs::emit_data(...);
    pty_.send_escape_sequence(body);
  }

  void shutdown() override {
    pty_.send_escape_sequence(dcs::kProtoVersion + ";type=END");
    pty_.stop();
  }
};
```

## 12. Public API Contract

### pty_server

```
void start(const std::string& command)
```
Spawns `command` inside a real PTY. On Linux, switches the calling process's
terminal to raw/no-echo mode so user keystrokes reach the child unmodified.
Starts the background loop thread. Throws `std::runtime_error` on failure.

```
void stop()
```
Sends SIGTERM / TerminateProcess to the subprocess, waits for exit, closes the
PTY, restores the caller's terminal mode, and joins the loop thread. Single-use.

```
bool is_running() const
```
Returns false once the subprocess exits or `stop()` has been called.

```
void send_input(const uint8_t* data, size_t n)
```
Thread-safe. Enqueues `n` bytes for writing to the PTY master (user keystrokes
or injected terminal input).

```
void inject_escape_sequence(const std::string& body)
```
Thread-safe. Wraps `body` in a DCS frame (`ESC P <body> ESC \`) and enqueues
it for writing to the PTY master.

### pty_client

```
void start(bool set_raw_mode = true)
```
Opens stdin/stdout via libuv (`uv_guess_handle` selects handle type). Starts
the loop thread and DCS parser. If `set_raw_mode` is true and stdin is a TTY,
switches to raw mode.

```
void stop()
```
Signals `stop_async`, joins the loop thread, and restores TTY mode if changed.

```
void send_escape_sequence(const std::string& body)
```
Thread-safe. Wraps `body` in a DCS frame and writes it to stdout.

```
void send_output(const uint8_t* data, size_t n)
```
Thread-safe. Writes raw bytes to stdout.

## 13. CMake Wiring

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(bison PRIVATE
    src/rm/pty/pty_server_linux.cpp
    src/rm/pty/pty_client.cpp
  )
  target_link_libraries(bison PRIVATE util)  # forkpty lives in libutil
elseif(WIN32)
  target_sources(bison PRIVATE
    src/rm/pty/pty_server_win.cpp
    src/rm/pty/pty_client.cpp
  )
  set_source_files_properties(src/rm/pty/pty_server_win.cpp
    PROPERTIES COMPILE_DEFINITIONS "_WIN32_WINNT=0x0A00")
endif()
target_link_libraries(bison PRIVATE uv_a)
```

## 14. Constraints and Invariants

- `pty_server` **must** use `forkpty` / `CreatePseudoConsole`. Using `uv_spawn`
  with pipes gives the child a non-TTY stdin and breaks interactive shells.
- The loop thread is the sole owner of all libuv handles. No handle may be
  touched from another thread except through `uv_async_send`.
- `stop()` must be called (or the destructor must call it) before the object is
  destroyed. Destructors call `stop()` as a safety net.
- Terminal raw mode (set by `pty_server::start()` on Linux) is restored in
  `stop()`. Abnormal exit without `stop()` leaves the terminal in raw mode;
  callers should install a SIGTERM/SIGINT handler that calls `stop()`.
- On Windows, the child-facing pipe ends (`hInRead`, `hOutWrite`) must be closed
  by the server process immediately after `CreatePseudoConsole` returns. Keeping
  them open prevents the ConPTY from detecting when the child exits.
- The PTY master fd/handle must remain open until `stop()` is called. Closing it
  prematurely sends EOF to the child on Linux and terminates the ConPTY session
  on Windows.

## 15. Integration Boundaries

**Depends on:**

- `libuv` (`uv_a`) — event loop, `uv_pipe_t`, `uv_tty_t`, `uv_async_t`.
- `src/rmi/transport/dcs_framing.hpp` — `dcs_byte_parser`, DCS constants,
  base64 codec. (Header-only; no link dependency.)
- Linux only: `libutil` (`-lutil`) for `forkpty`.

**Depended on by:**

- `src/rmi/transport/pty_server_transport` — inherits `pty_server`, overrides
  `on_escape_sequence` to implement `server_transport_iface`.
- `src/rmi/transport/pty_client_transport` — inherits `pty_client`, overrides
  `on_escape_sequence` to implement `client_transport_iface`.
- Any code that needs PTY interception without the RMI layer.
