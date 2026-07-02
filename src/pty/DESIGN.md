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

**`raw_mode_guard`** — RAII helper, unrelated to `pty_process`'s fork/exec
lifecycle, that saves an fd's terminal mode and switches it to raw mode for
the guard's lifetime, restoring the original mode on destruction. Used by
`client_app`'s `--pty` path (`src/app/client/client_app.cpp`) on its own
fd 0 — see Design Decisions below for why. Same opaque-state-per-platform
split as `pty_process`; the Windows translation unit is a permanent no-op
stub (construction never fails, it just does nothing), since the client's
`--pty` mode is only reachable in practice from a Linux `pty_process`
session.

**`crlf_output_guard`** — RAII helper that rewrites `std::cout`/`std::cerr`'s
`'\n'` to `"\r\n"` for its lifetime. Compensates for `raw_mode_guard` turning
`OPOST` off (see Design Decisions below for why that's necessary and why it's
a problem for ordinary text output). Pure `std::streambuf`, no OS calls, so —
unlike `raw_mode_guard` and `pty_process` — it has no Linux/Windows split.

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
- **The pty slave's cooked termios corrupts `BISON:` framing, so the client
  goes raw instead of the server.** The slave keeps its default (cooked)
  termios so the spawned shell behaves normally — but that same cooked mode
  applies to *any* process attached to the slave, including a `bison_cli
  --pty` launched from inside that shell. Two cooked-mode behaviors break
  the framing:
  - `OPOST`/`ONLCR` rewrites every `\n` the client writes into `\r\n` on its
    way out to the master, so the stray `\r` lands inside the frame payload
    right before the closing `\n` and fails to base64-decode.
  - `ECHO` on the slave loops the server's own outgoing frame bytes (written
    into the master, which the pty treats as "terminal input" to the slave)
    straight back out through the master, so the server's own reader sees
    its own frame echoed back at it.

  Both are fixed the same way `less`/`vim`/`ssh` fix it: the client, which
  is the sole foreground reader/writer of the slave for the duration of its
  RMI session, calls `tcsetattr`/`cfmakeraw` on fd 0 via `raw_mode_guard`
  before opening `stdio_client_transport`, and restores the original (cooked)
  termios on exit so the parent shell keeps behaving normally afterwards.
  This was chosen over changing the *slave*'s termios from the server side
  because the server has no fd onto the slave at all — only `master_fd()` —
  and changing cooked-mode behavior globally for the pty would break the
  shell's own interactivity while the operator is just using it as a normal
  shell (no client attached).
- **Raw mode breaks two more things client-side, fixed in `client_app`, not
  here.** `raw_mode_guard` only owns the termios change; the fallout from
  that change is `client_app`'s problem to fix, since only it knows what
  else is reading/writing the fd:
  - Turning `ECHO` off is required (see above) but also means the operator's
    own keystrokes are never echoed anywhere — nothing appears on screen
    while typing, even though the REPL receives them correctly. This looks
    identical to a hang from the operator's seat. `client_app` re-implements
    just that echo in software (`feed_console_passthrough`, with basic
    `0x7f`/`0x08` backspace handling so the internal line buffer matches
    what's on screen).
  - Turning `OPOST` off is required (see above) but also strips `\r` from
    *all* output on that fd, not just frame writes — including ordinary
    `std::cout`/`std::cerr` text. Real terminals don't auto-return on a bare
    `\n`, so without correction, output stairsteps down and to the right.
    `client_app` compensates with `crlf_output_guard`.

  Both fixes write to the same fd 1 that the transport's own writer already
  owns (frames going out via `send()`). A local-echo or `crlf_output_guard`
  implementation that wrote to fd 1 directly (bypassing the transport) would
  race that writer at the byte level — two independent, unsynchronized
  writers on one fd is exactly the class of bug this whole file has been
  about. `client_app` avoids it by giving `crlf_output_guard` a *sink*
  (`stdio_client_transport::send_raw()`) instead of letting it write fd 1
  directly, so echo, `send_raw`-routed text, and frames all funnel through
  the transport's one synchronized writer queue. (This is also why the
  equivalent server-side fix — for `pty_process`'s own real-terminal raw
  mode stripping `\r` from `server_app`'s status messages — isn't
  implemented: `stdio_server_transport`'s passthrough callback forwards
  bytes it reads *verbatim*, including the client's already-`crlf`-corrected
  text, straight through `std::cout`; wrapping the server's `std::cout`
  globally would double the `\r` on anything forwarded that way. Fixing the
  server's own two or three status lines isn't worth that complexity, so
  they're left stairstepping — a known, minor, currently-unfixed
  limitation.)

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
- `src/app/client/client_app.cpp` — owns a `raw_mode_guard` and a
  `crlf_output_guard` on its own fd 0/1 in `--pty` mode, for the lifetime of
  the RMI session. This is the client side of the same `--pty` feature but
  does not depend on `pty_process` — see the termios note in Design
  Decisions above for why the two are separate abstractions. Constructs
  `crlf_output_guard` with a sink routed through
  `stdio_client_transport::send_raw()`, not the default (direct-to-fd)
  constructor — see the single-writer note in Design Decisions above.
- `examples/rmi_client_example.cpp` — a second, minimal `--pty` client (no
  REPL, no local echo) used to validate the transport in isolation from
  `cli_app`'s REPL machinery. Uses `raw_mode_guard` and the default
  (direct-to-fd) `crlf_output_guard` constructor — safe here specifically
  because this example has no background-thread writer to fd 1 to race
  against (no local echo, no console-passthrough queue).
- `examples/rmi_server_example.cpp` — a second, minimal `--pty` server,
  mirroring `server_app.cpp`'s `--pty` branch (`pty_process` +
  `stdio_server_transport`) without the rest of `server_app`'s
  flag-parsing/hook scaffolding.
