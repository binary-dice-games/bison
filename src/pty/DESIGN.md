# PTY Server/Client Design

## 1. Purpose and Scope

`src/pty` implements a PTY-backed bison RMI channel that works over interactive
shell links (SSH, adb shell, serial consoles) without opening extra ports. The
server is a PTY emulator: it launches a shell subprocess, relays the user's
terminal I/O normally, and simultaneously uses the same PTY stream as the
bison transport channel. The client runs on the remote machine and uses its
own stdin/stdout as the bison transport.

This directory does NOT implement:

- The core stdio transport framing — that lives in `src/rmi/transport/stdio_transport`.
- The generic RMI server/client runtime — that lives in `src/rmi/server` and `src/rmi/client`.

Source files owned by this directory:

- `pty_server_transport.hpp` / `pty_server_transport.cpp` — PTY-owning server transport.
- `pty_server_app.hpp` / `pty_server_app.cpp` — multi-session server application scaffold.
- `pty_client_app.hpp` / `pty_client_app.cpp` — remote client application scaffold.

Linux/POSIX only. All files are guarded with `#if defined(__linux__)`.

## 2. Design Goals

1. The server is a usable terminal emulator at all times — the user sees
   normal shell output, can type commands, run SSH, etc., whether or not a
   bison client is connected.
2. The bison channel shares the PTY stream transparently — no extra sockets,
   ports, or out-of-band channels are required.
3. Multiple sequential client sessions are supported — after one client
   disconnects the server stays alive, re-emits HELLO, and accepts the next
   client.
4. All objects created during a session are destroyed when that session ends —
   the next client starts with a clean server state.
5. Both `pty_server_app` and `pty_client_app` are extensible base classes.
   Applications subclass them, override virtual hooks, and call `run()`. A
   calculator server, for example, overrides `register_classes()` and
   `on_session()`; a calculator client overrides `on_session()` to drive the
   remote calculator object.

## 3. Key Abstractions

### pty_server_transport

A `server_transport_iface` implementation that owns the PTY. It integrates
two responsibilities that cannot be separated:

- **Terminal relay**: reads PTY master output, forwards plaintext bytes to
  `stdout` (so the user sees the terminal), extracts DCS frames for bison.
  Reads user `stdin` in raw mode and writes keystrokes to the PTY master
  (interleaved with outgoing bison frames under the same write lock).
- **Bison transport**: delivers extracted DCS frames as a queue of bison
  buffers; `accept()` returns one connection per session; `send()` writes DCS
  frames back through the PTY master.

Invariants:

- Exactly one reader thread owns the PTY master read fd for the lifetime of
  the transport.
- Exactly one input-relay thread owns user `stdin` for the lifetime of the
  transport.
- All writes to the PTY master fd go through a single `write_mtx` — bison
  frame sends and user keystroke relay never interleave.
- The reader thread is never joined or detached between sessions; it runs
  continuously until `stop()`.

### pty_server_app

An extensible base class that manages the multi-session loop on top of
`pty_server_transport`. Concrete applications subclass it, override
`register_classes()` to expose their domain objects, and optionally override
`on_session()` to run per-session logic after the client connects. The base
class handles:

- Calling `transport.start()` once.
- Looping: `accept()` → create `rmi::server` → serve until session ends →
  destroy `rmi::server` (which destroys all session objects) →
  `transport.restart_session()` → repeat.
- Stopping cleanly when the shell subprocess exits.

### pty_client_app

An extensible base class for processes running on the remote machine. It uses
the process's own `stdin`/`stdout` as the bison transport — no subprocess is
launched. Concrete applications override `on_session()` to interact with the
server's remote objects (instantiate, call, get/set, etc.). The base class
handles transport construction, handshake, and disconnect.

## 4. Data Flow and Architecture

### Steady state — no bison client connected

```
user kbd → [raw stdin] → input-relay thread → PTY master write
PTY slave (bash/ssh) stdout → PTY master read → reader thread
  └─ plaintext bytes ──────────────────────────→ stdout (user sees terminal)
  └─ DCS frames ──────────────────────────────→ inbox queue (waiting for HELLO)
```

### Active bison session

```
user kbd → input-relay thread → PTY master write  ← bison frame sends (write_mtx)
PTY slave stdout → reader thread
  └─ plaintext bytes → stdout
  └─ DCS frames ──→ inbox queue → rmi::server worker
                                      └─ responses → send() → PTY master write
```

The remote side (after SSH):

```
pty_client stdin ←── SSH channel ←── PTY slave stdin ←── PTY master write
pty_client stdout ──→ SSH channel ──→ PTY slave stdout ──→ PTY master read
```

### Session lifecycle state machine

```
IDLE ──start()──→ WAITING ──HELLO from client──→ CONNECTED
                     ↑                                  │
                     └──── restart_session() ←──────────┘
                                                  (END frame or shell exit)
WAITING/CONNECTED ──stop()──→ STOPPED
```

In `WAITING`, the transport has already emitted a HELLO frame into the PTY so
any client that connects can complete the handshake.

In `CONNECTED`, the `rmi::server` owns the connection. When the connection
closes (END frame received, or the client disappears), the caller destroys
the `rmi::server` instance — which destroys the session context and all
remote objects — then calls `restart_session()`.

## 5. Public API Contract

### pty_server_transport

```
pty_server_transport(std::string shell = "bash")
```

Constructs the transport with the given shell command. Does not launch the
shell yet.

```
void start(bison::dynamic params) override
```

Forks the shell via `forkpty`, sets the caller's terminal to raw/no-echo
mode, starts the reader thread and input-relay thread, emits a HELLO frame.
Throws `std::runtime_error` if `forkpty` fails or the transport was already
started.

```
std::unique_ptr<server_connection_iface> accept(milliseconds timeout) override
```

Blocks until a HELLO frame arrives from the client side, then returns a
connection. Returns `nullptr` on timeout or if the shell has exited. After
the first call succeeds, subsequent calls return `nullptr` until
`restart_session()` resets the state.

```
void stop() override
```

Sends `exit\n` to the shell, closes the PTY master fd, restores the user's
terminal mode, and detaches both background threads. The transport is
single-use after `stop()` — do not call `start()` again on the same instance.

```
void restart_session()
```

Clears the inbox, resets the `hello_seen` and `closed` atomics, resets the
accepted flag so `accept()` can return a new connection, and re-emits a HELLO
frame. Does not touch the reader or input-relay threads. Must only be called
between sessions (after the previous `rmi::server` has been destroyed).

```
bool is_shell_running() const
```

Returns `false` once the shell subprocess has exited. The caller's main loop
uses this to decide when to break out of the session loop.

```
bool wait_until_closed(milliseconds timeout) const
```

Blocks until the current session connection is closed or timeout elapses.
Used by `pty_server_app` to wait for session end without busy-polling.

### pty_server_app

```
int run(int argc, char** argv)
```

Calls `register_classes()`, starts the transport, then loops:
  1. Call `transport.accept()`.
  2. If shell has exited, break.
  3. Construct `rmi::server(transport)`, call `srv.listen()`.
  4. Call `on_client_connected()`.
  5. Wait for `transport.wait_until_closed()`.
  6. Destroy `srv` (destroys all session objects).
  7. Call `on_session_ended()`.
  8. Call `transport.restart_session()`.

Returns 0 on clean shell exit, 1 on error.

Protected virtual hooks — all have default no-op or default-value
implementations, so subclasses only override what they need:

```
virtual void register_classes() = 0
virtual std::string shell_command() const     // default: "bash"
virtual bison::dynamic listen_params() const  // default: mode=dcs
virtual void on_client_connected() const
virtual void on_session_ended() const
virtual void on_error(const std::string& msg) const
```

### pty_client_app

```
int run(int argc, char** argv)
```

Creates `stdio_client_transport()` (default constructor — uses process
`stdin`/`stdout`), calls `connect()`, calls `on_session()`, calls
`disconnect()`. Returns the value from `on_session()`, or 1 on error.

Protected virtual hooks:

```
virtual int on_session(rmi::client& c) = 0
virtual void on_connected() const
virtual void on_error(const std::string& msg) const
virtual void on_connect_params(bison::dynamic& params) const
```

`on_connect_params` is called before `connect()` and sets defaults:
`mode=dcs`, `handshake_timeout_ms=300000` (five minutes — allows the user
time to SSH and start the client before the handshake times out).

## 6. Design Decisions

**Server is the PTY emulator, not the client.**
The original `pty_client_app` / `pty_server_app` in `src/rmi` had the client
launch the subprocess and the server run passively on stdin/stdout. That
topology does not support the SSH use case: the server must be the PTY
emulator so it can remain running across multiple SSH sessions while the
client is a transient remote process.

**Both app scaffolds are extensible base classes, not standalone executables.**
This mirrors how `src/rmi/server/server` and `src/rmi/client/client` are
used: the application provides the domain logic; the scaffold provides
lifecycle management. A calculator app, for example, subclasses
`pty_server_app`, overrides `register_classes()` to register the calculator
class, and optionally overrides `on_session()`. The client subclasses
`pty_client_app` and overrides `on_session()` to instantiate and drive the
remote calculator object.

**Single reader thread across sessions.**
`stdio_server_transport` detaches its reader thread on `stop()` (because it
may be blocked in a `read` syscall) and cannot be restarted. For the PTY case
the PTY master fd persists across sessions, so the reader must keep running.
`pty_server_transport` never stops the reader between sessions; it only resets
inbox state. The reader is detached only during the final `stop()`.

**Plaintext relay to stdout, not stderr.**
`stdio_server_transport` optionally mirrors non-frame bytes to `stderr`.
For PTY use, plaintext must go to `stdout` so the user sees the terminal
normally. The reader loop in `pty_server_transport` writes non-DCS bytes
directly to `stdout` at byte granularity rather than line-buffered.

**DCS mode only, no line-mode fallback.**
The PTY master fd is a local in-process channel — it does not pass through
any relay that would strip DCS sequences. Line-mode detection adds parser
complexity and would interfere with the `@@BISON_RMI@@` prefix appearing in
shell output. `pty_server_transport` forces `mode=dcs`.

**New `rmi::server` per session, not per process.**
Creating a fresh `rmi::server` for each session is the simplest way to
guarantee that all previous-session objects are destroyed: destroying the
`server` tears down the session context. Classes are registered once
(before the session loop) through the global bison class registry, so they
persist across sessions.

**Server emits HELLO on `start()` and after each `restart_session()`.**
The client waits for HELLO before connecting (matching
`stdio_client_transport` handshake behavior). Emitting HELLO at the start of
the wait window means any client that appears can complete the handshake
without the server polling or emitting periodically.

## 7. Constraints and Invariants

- Linux only. `forkpty`, `STDIN_FILENO`, `termios`, `poll` are used directly.
- The PTY master fd is never closed between sessions. Closing it would
  signal EOF to the shell subprocess.
- `write_mtx` must be held for every write to the PTY master fd. User
  keystrokes and bison frame sends share this lock.
- `restart_session()` must not be called while a session is active (i.e.,
  while an `rmi::server` instance is alive over this transport).
- The reader thread must never read from the PTY master fd after `stop()` sets
  `stop_requested`; it exits the loop and is detached from `stop()`.
- User terminal mode (raw/no-echo) is enabled in `start()` and restored in
  `stop()`. Abnormal termination without calling `stop()` will leave the
  terminal in raw mode; callers should install a signal handler that calls
  `stop()` on `SIGTERM`/`SIGINT`.
- All exceptions from bison RMI operations are caught by the `rmi::server`
  worker threads. `pty_server_app` only catches transport-level exceptions.

## 8. Integration Boundaries

Depends on:

- `src/rmi/transport/stdio_transport` — `stdio_client_transport` is used by
  `pty_client_app`. The DCS framing constants and base64 codec in
  `stdio_transport.cpp` are not reused directly; `pty_server_transport`
  reimplements the DCS byte-level state machine to gain control over plaintext
  routing.
- `src/rmi/server/server` — `pty_server_app` creates an `rmi::server` per
  session.
- `src/rmi/client/client` — `pty_client_app` creates an `rmi::client`.
- `src/core/bison.hpp` — `bison::dynamic` for params and class registration.

Depended on by:

- Application binaries that subclass `pty_server_app` or `pty_client_app` to
  expose domain objects over a PTY/SSH channel.
- Nothing in `src/rmi` depends on `src/pty`.
