# `src/pty` — Design

## Purpose / Scope

Provides `pty_process`, an RAII wrapper around a forked pseudo-terminal
session used by `bison_server --pty` to give an operator a real, fully
interactive terminal (`isatty() == true`) that a bison RMI session can be
tunneled through. This directory only owns the pty/process lifecycle; the
`BISON:`-line framing that rides on top of the pty master fd lives in
`src/rmi/transport/stdio_transport.hpp` (a separate, cross-platform
concern — see Integration Boundaries below).

## Design Goals

- The spawned terminal must be genuinely usable: normal shell interaction
  (tab completion, job control, `Ctrl-C`, prompts) has to work exactly as
  it would in any other terminal emulator.
- No `#ifdef`-based platform branching (per the repo-wide coding style) —
  Linux and Windows implementations are separate translation units sharing
  one platform-neutral header.
- Minimal shared state between the pump thread and the owning thread.

## Key Abstractions

**`pty_process`** — forks a child (default `$SHELL`, or an explicit command)
attached to a new pty. Exposes:
- `master_fd()` — the pty master fd. This is the *only* fd another
  component needs to build a transport on top of.
- `start_pump()` — begins forwarding the operator's real stdin into the pty
  master, byte for byte.
- `wait()` — blocks for child exit, used by `server_app` as its shutdown
  condition in `--pty` mode instead of `std::getline(std::cin, ...)`.

Platform-specific state (a saved `struct termios`, the pump thread's
shutdown `eventfd`) is kept behind an opaque, forward-declared
`pty_process_state`, defined separately in `pty_process_linux.cpp` and
`pty_process_win.cpp`, so the shared header stays platform-neutral.

## Data Flow

```
operator's real terminal (fd 0)          pty master (pty_process::master_fd())
        │  raw-mode read()                        │
        ▼                                          │
  pump_loop() ──── write() ───────────────────────►│
                                                    │  (pty slave, default
                                                    │   termios, execs the
                                                    │   child shell)
        ▲                                          │
        │        stdio_server_transport reads ◄────┘
        │        this direction and either
        │        decodes BISON: frames or
        └──────  passes bytes straight through
                 to the console (this is how the
                 child's own echo of the operator's
                 keystrokes becomes visible again)
```

`pty_process` only ever pumps in the operator→pty direction. The pty
master's *output* direction has exactly one reader:
`stdio_server_transport` (see `src/rmi/transport/stdio_transport.hpp`).
This single-reader invariant is intentional — a second reader competing for
bytes off the master fd would race with the transport's `BISON:`-prefix
scanner and corrupt both the RMI frames and the operator's view of the
terminal.

## Design Decisions

- **Unidirectional pump.** See Data Flow above. Keeping `pty_process`
  ignorant of the master fd's read side means it has no framing knowledge
  at all — it is a pure pty/process lifecycle primitive, and
  `stdio_transport` (a different module) owns the framing. This keeps the
  two concerns cleanly separated and testable independently.
- **Raw mode applies to the operator's real terminal, not the pty slave.**
  `cfmakeraw()` is called on fd 0 (this process's own controlling
  terminal), not on the spawned pty. This is what makes `pump_loop()`'s
  blocking `read()` deliver keystrokes immediately (no `ICANON` line
  buffering) and forward `Ctrl-C` as a literal `0x03` byte instead of
  raising `SIGINT` against this process (`ISIG` is off on the real
  terminal). The pty slave keeps its default termios, so the kernel's line
  discipline on that side converts the forwarded `0x03` into a proper
  `SIGINT` for the child — exactly the behavior an operator expects from an
  interactive shell.
- **Linux-only.** Implemented via `forkpty()`. Windows has no equivalent
  primitive without a substantially different implementation (ConPTY), so
  `pty_process_win.cpp`'s constructor throws `std::runtime_error`
  unconditionally rather than half-implementing something unusable.

## Constraints / Invariants

- Exactly one reader of `master_fd()`'s output direction at any time (see
  Data Flow). `pty_process` itself never reads it.
- The real terminal's original termios state is saved at construction and
  restored at destruction — a crash between those two points (e.g. an
  uncaught exception unwinding past this object) leaves the operator's
  shell in raw mode; this is an accepted risk shared with most raw-mode
  terminal tools.

## Integration Boundaries

- `src/rmi/transport/stdio_transport.hpp` — depends on this module for
  `master_fd()` on the server side; wraps it in the `BISON:`-line framing.
  The client side of `stdio_transport` does not depend on this module at
  all (it wraps the client process's own already-connected stdio, no pty
  involved — see `src/app/client/client_app.cpp`).
- `src/app/server/server_app.cpp` — owns the `pty_process` instance in
  `--pty` mode, calls `start_pump()`, and blocks on `wait()` as its
  run-loop shutdown condition.
