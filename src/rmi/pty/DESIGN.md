# PTY Abstraction Layer Design

## 1. Purpose and Scope

`src/rmi/pty` provides the OS-level pseudoterminal process factory used by the bison RMI
transport layer. It is responsible for spawning a child process inside a real PTY on Linux
(`forkpty`) and Windows (ConPTY), and exposing the resulting native I/O handles for the
transport adapter to consume.

**Core invariant:** the spawned child process must see `isatty(stdin) == true`. This is the
reason the entire PTY mechanism exists — any program that calls `isatty()` (shells, REPLs,
readline, the bison RMI client itself) will behave as if attached to a real terminal.

The module has two components split across two directories:

- `src/rmi/pty/` — the OS-level PTY process wrapper (`pty_process`). No libuv, no DCS
  framing, no RMI knowledge. Its sole job is to allocate the PTY and spawn the child.
- `src/rmi/transport/` — the transport adapters (`pty_server_transport`,
  `pty_client_transport`) that consume the handles produced by `pty_process` and implement
  the `transport_iface` contracts using libuv and DCS framing.

---

## 2. Why a Real PTY is Required

Shells and interactive programs call `isatty(stdin)` on startup to detect whether they are
attached to a terminal. When the answer is false — which is always the case with ordinary
anonymous pipes — they permanently disable readline editing, color output, ANSI escape
sequences, prompts, and job control.

The bison RMI client transport also relies on `isatty()` being true to use `uv_pipe_open`
on a PTY slave fd without triggering pipe-mode fallbacks in the application or any library
it links against.

The fix is `pty_process::spawn()`, which allocates a master/slave PTY pair and connects
the child's stdin/stdout/stderr to the slave:

- **Linux:** `forkpty()` does this atomically — it forks, connects the child to the PTY
  slave, and returns the master fd to the parent.
- **Windows:** `CreatePseudoConsole()` allocates the PTY; `CreateProcess()` is called with
  `STARTUPINFOEX` so the child inherits the ConPTY as its console.

In both cases, `isatty(0)` returns `true` in the child.

---

## 3. Two-Component Architecture

```
src/rmi/pty/                        src/rmi/transport/
────────────────────────────        ───────────────────────────────────────────
pty_process                         pty_server_transport
  spawn(cmd, args)        ──────►     ├── owns pty_process instance
  → master_fd (Linux)                 ├── runs uv_run() on background thread
  → h_in_write /                      ├── reads PTY master:
    h_out_read (Win)                  │     DCS frame  → rmi::server inbox
                                      │     plain byte  → on_plain callback
                                      └── writes PTY master:
                                            DCS frames ← rmi::server outbox

                                    pty_client_transport
                                      ├── no pty_process — the process's own
                                      │   stdin/stdout ARE the PTY slave
                                      ├── runs uv_run() on background thread
                                      ├── reads stdin  via uv_pipe_t:
                                      │     DCS frame  → rmi::client inbox
                                      │     plain byte  → discarded / on_plain
                                      └── writes stdout via uv_pipe_t:
                                            DCS frames ← rmi::client outbox
```

The PTY layer is a pure handle factory. All async I/O machinery (libuv loop, send queue,
receive queue, DCS parser) lives entirely inside the transport adapters.

---

## 4. Data Flow

```
Server side (pty_server_transport)        Child process (PTY slave)
──────────────────────────────────        ──────────────────────────────────
uv_read_start on master fd                stdin  [isatty == true ✓]
  │                                         │
  └─► dcs_byte_parser::feed()              rmi client writes DCS frame → stdout
        ├─ DCS frame → inbox queue
        └─ plain byte → on_plain cb
                                          stdout [isatty == true ✓]
uv_write to master fd                       │
  │                                       reads DCS frames from stdin
  └─► emit_data()                         via uv_pipe_open(loop, &h, STDIN_FILENO)
        ← send queue drain (uv_async_t)
```

Plain bytes flowing out of the PTY master (shell prompts, command output) are routed to the
`on_plain` callback supplied at construction time. The caller is responsible for forwarding
them to the appropriate output channel (the server's own terminal, an SSH channel, etc.).

---

## 5. Deployment Scenario

The primary use case is **interactive shell with embedded RMI client**:

```
Terminal / SSH channel ◄────────────────────────────────► user
                 │
      pty_server_transport  (RMI server process)
                 │  PTY master fd
                 │
          [ forkpty / ConPTY ]
                 │  PTY slave
                 │
           bash / shell   (isatty(stdin) == true ✓)
                 │
           rmi client     (isatty(stdin) == true ✓)
                ├── reads DCS frames from stdin
                └── writes DCS frames to stdout
```

The user interacts with bash normally. When the user launches the bison RMI client binary
from that shell, it inherits the PTY slave as its stdin/stdout. `pty_client_transport`
wraps those descriptors and multiplexes DCS frames alongside any plain terminal I/O.

---

## 6. Implementation Files

| File | Location | Role |
|---|---|---|
| `pty_process.hpp` | `src/rmi/pty/` | Public interface: `spawn()`, handle accessors, move-only `pty_process` type |
| `pty_process_linux.cpp` | `src/rmi/pty/` | `forkpty()` implementation |
| `pty_process_win.cpp` | `src/rmi/pty/` | `CreatePseudoConsole()` + `CreateProcess()` implementation |
| `pty_server_transport.hpp/cpp` | `src/rmi/transport/` | Server adapter: owns `pty_process`, drives libuv loop, DCS framing |
| `pty_client_transport.hpp/cpp` | `src/rmi/transport/` | Client adapter: wraps stdin/stdout via `uv_pipe_t`, DCS framing |

---

## 7. libuv Handle Binding

Once `pty_process::spawn()` returns, the transport adapter binds the handles to libuv:

- **Linux server:** `uv_pipe_open(&pipe, master_fd)` — the master fd is bidirectional;
  one `uv_pipe_t` handles both reads and writes.
- **Windows server:** ConPTY exposes two separate pipe ends. `h_out_read` is opened on a
  read-only `uv_pipe_t`; `h_in_write` is opened on a write-only `uv_pipe_t`. Two handles
  are required because Windows provides no single bidirectional descriptor here.
- **Client (both platforms):** `uv_pipe_open(&pipe, STDIN_FILENO)` for reads and
  `uv_pipe_open(&pipe, STDOUT_FILENO)` for writes. `uv_tty_t` is not used — the client
  transport needs only byte-stream I/O for DCS parsing, not TTY control features. Using
  `uv_pipe_t` on a PTY slave fd is valid and gives the necessary raw byte access.

The async-to-sync bridge pattern follows the same structure as all other libuv-backed
transports in `src/rmi/transport/`:

- Background thread runs `uv_run(&loop, UV_RUN_DEFAULT)`.
- Inbound DCS frames are pushed to a `synchronized<queue<buffer>>` and signal a condition
  variable; `receive()` blocks on that CV.
- Outbound frames are pushed to a send queue and wake the loop via `uv_async_send`.
- Shutdown sets a stop flag and calls `uv_async_send` on a stop handle; all handles are
  closed in the callback and the loop exits, joining the background thread.

---

## 8. Escape-Sequence Protocol

DCS frames embed bison RMI messages in the terminal stream as `ESC P <body> ESC \`. The
transport adapters instantiate `dcs::dcs_byte_parser` locally and feed each byte from the
PTY master (server side) or stdin (client side) into it. The parser fires:

- `on_frame(body)` for each complete DCS block — routed to the RMI layer.
- `on_plain(byte)` for bytes outside DCS blocks — forwarded by the caller or discarded.

Outbound RMI frames are split into chunks, base64-encoded, and emitted as one or more DATA
DCS blocks via `dcs::emit_data()`. All helpers live in
`src/rmi/transport/dcs_framing.hpp`.

---

## 9. Design Goals

1. **Real PTY, real terminal** — the spawned child always has `isatty(stdin) == true`.
2. **Strict layer separation** — `pty_process` has zero knowledge of libuv, DCS framing,
   or the RMI protocol. It is a handle factory, nothing more.
3. **Consistent async-to-sync bridge** — follows the same background-thread/CV pattern as
   all other libuv-backed transports; no novel concurrency model.
4. **Client needs no PTY machinery** — `pty_client_transport` wraps the file descriptors
   it already has (stdin/stdout); it never creates or manages a PTY itself.
