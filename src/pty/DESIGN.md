# `src/pty` — Design

## Purpose / Scope

Provides `pty_process`, an RAII wrapper around a forked pseudo-terminal
session used by `bison_server --pty` to give an operator a real, fully
interactive terminal (`isatty() == true`) that a bison RMI session can be
tunneled through. This directory only owns the pty/process lifecycle; the
`BISON<...>` framing that rides on top of the pty master fd lives in
`src/rmi/transport/stdio_transport.hpp` (a separate, cross-platform
concern — see Integration Boundaries below).

## Design Goals

- The spawned terminal must be genuinely usable: normal shell interaction
  (tab completion, job control, `Ctrl-C`, prompts) has to work exactly as
  it would in any other terminal emulator.
- Linux and MSYS2 only — both share one implementation (per the repo-wide
  coding style, `#ifdef`-based branching is reserved for the narrow
  in-file cases noted below, e.g. `eventfd` vs. self-pipe).
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

State (a saved `struct termios`, the pump thread's shutdown `eventfd`) is
kept behind an opaque, forward-declared `pty_process_state`, defined in
`pty_process.cpp`, so the shared header stays free of `<termios.h>`/etc.

**`raw_mode_guard`** — RAII helper, unrelated to `pty_process`'s fork/exec
lifecycle, that saves an fd's terminal mode and switches it to raw mode for
the guard's lifetime, restoring the original mode on destruction. Used by
`client_app`'s `--pty` path (`src/app/client/client_app.cpp`) on its own
fd 0 — see Design Decisions below for why. Same opaque-state pattern as
`pty_process`, defined in `raw_mode_guard.cpp`.

**`crlf_output_guard`** — RAII helper that rewrites `std::cout`/`std::cerr`'s
`'\n'` to `"\r\n"` for its lifetime. Compensates for `raw_mode_guard` turning
`OPOST` off (see Design Decisions below for why that's necessary and why it's
a problem for ordinary text output). Pure `std::streambuf`, no OS calls, so —
unlike `raw_mode_guard` and `pty_process` — it needs no OS-specific state.
Client-side only — see `pty_write` below for why the server side needs a
different fix for the same underlying problem.

**`pty_write`** (`to_crlf()` + `write_raw()`) — the server-side equivalent of
`crlf_output_guard`: `to_crlf()` does the same `'\n'` → `"\r\n"` string
rewrite, and `write_raw()` writes the result directly to an fd with a plain
POSIX `write()` loop. Used by
`server_app::on_listening()`/`on_verbose_trace()`, in preference to
`crlf_output_guard`, specifically because those hooks share fd 1 with
`stdio_print_passthrough`'s background-thread writes — see Design Decisions
below.

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
        │        decodes BISON<...> frames or
        └──────  passes bytes straight through
                 to the console (this is how the
                 child's own echo of the operator's
                 keystrokes becomes visible again)
```

`pty_process` only ever pumps in the operator→pty direction. The pty
master's *output* direction has exactly one reader:
`stdio_server_transport` (see `src/rmi/transport/stdio_transport.hpp`).
This single-reader invariant is intentional — a second reader competing for
bytes off the master fd would race with the transport's `BISON<...>`-prefix
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
- **Linux and MSYS2 only.** Implemented via `forkpty()`, which
  `pty_process.cpp` uses for both native Linux and MSYS2 builds
  (`CMAKE_SYSTEM_NAME STREQUAL "MSYS"`), since MSYS2 provides the same POSIX
  pty layer, modulo a couple of `#if !defined(__linux__)` branches (a
  self-pipe instead of `eventfd`, which MSYS2 doesn't have).
- **On MSYS2, `--pty`-capable executables must be launched from an MSYS2
  MSYS shell** (not MinGW64, not a plain `cmd.exe`/PowerShell prompt) — see
  `docs/building.md`. MSYS2's `msys-2.0.dll` derives its POSIX path-mount
  table (including the virtual `/bin` → `/usr/bin` alias that
  `execl("/bin/sh", ...)` depends on) from wherever that DLL itself was
  loaded from. If it's loaded from anywhere other than a real `usr/bin`
  under the MSYS2 root — e.g. a copy sitting next to the built `.exe` — the
  mount table can't be built, `/bin/sh` fails to resolve, `execl` fails with
  `ENOENT`, and the forked child immediately exits with status 127. From the
  outside this looks exactly like the whole program crashing right after
  printing its startup banner (a console flashing open and closed), since
  `pty_process::wait()` returns immediately once the never-really-started
  child is gone. For this reason build outputs do **not** ship a local copy
  of the MSYS runtime DLLs (there was a `cmake/CopyMsysRuntimeDeps.cmake`
  step doing exactly that; it was removed for this reason) — a shadowing
  local copy is worse than no copy at all, since it silently breaks
  `--pty` while every other code path keeps working. Rely on `PATH` from a
  real MSYS2 shell to resolve the runtime DLLs instead.
- **The pty slave's cooked termios breaks a client attached there, so the
  client goes raw instead of the server.** The slave keeps its default
  (cooked) termios so the spawned shell behaves normally — but that same
  cooked mode applies to *any* process attached to the slave, including a
  `bison_cli --pty` launched from inside that shell. Two cooked-mode
  behaviors are a problem for a client sharing that slave:
  - `ICANON` line-buffers input until a `\n`/`EOF` is seen. `BISON<...>`
    frames are terminated by `>`, not `\n` (see
    `src/rmi/transport/stdio_transport.hpp`), so under `ICANON` a frame
    would sit in the kernel's line buffer forever, since no `\n` ever
    arrives to release it.
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
  shell (no client attached). `cfmakeraw` also disables `OPOST` as a side
  effect of clearing those two — not needed for frame safety itself, since a
  `BISON<...>` frame never contains a `\n` for `OPOST`/`ONLCR` to mangle —
  but that side effect still strips `\r` from *all* other output on the fd,
  which is why `crlf_output_guard`/`pty_write` (below) remain necessary.
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
  (`stdio_client_transport::send()`) instead of letting it write fd 1
  directly, so echo, `send`-routed text, and frames all funnel through
  the transport's one synchronized writer queue.
- **The server side needs the same `\r` fix, but `crlf_output_guard` isn't
  safe there — `pty_write.hpp` is the server-side equivalent.**
  `pty_process`'s constructor puts the *operator's own* real terminal in raw
  mode too (for `pump_loop()`), which strips `\r` from `server_app`'s own
  `std::cout` status messages the exact same way. But redirecting
  `std::cout`'s streambuf the way `crlf_output_guard`'s default constructor
  does isn't safe here: `stdio_server_transport`'s passthrough callback
  (`stdio_print_passthrough`) *also* writes to `std::cout`, from the
  reader's background thread, to forward pty-master bytes — shell output,
  or a `--pty` client's own already-`crlf`-corrected text — **verbatim**.
  Redirecting `std::cout` globally would (a) double the `\r` on anything
  forwarded that way (confirmed: this is exactly the bug that showed up when
  first tried — client text arrived pre-corrected, then got corrected
  *again* on the way through `stdio_print_passthrough`) and (b) race that
  thread's writes, since swapping a stream's streambuf concurrently with
  another thread's `<<` on the same stream isn't safe.

  `server_app::on_listening()`/`on_verbose_trace()` instead use
  `pty::write_raw()` (`src/pty/pty_write.hpp`): a plain, synchronous
  `write()` to fd 1, bypassing `std::cout` entirely, so there's no shared
  mutable stream state to race and nothing to double-process (this write
  path and `stdio_print_passthrough`'s are now two independent syscall-level
  writers to the same fd, not two writers contending over one streambuf —
  safe, matching how concurrent `write()`s to a tty already behave). Safe to
  use unconditionally in these two hooks specifically because both are only
  ever reachable *after* `run()`'s `--pty` branch has already constructed
  `pty_proc` (raw mode confirmed active) — `on_error()` deliberately keeps
  writing to `std::cerr` unfixed, since it can also fire *before* that (e.g.
  `--pty` combined with `--host`/`--port`/`--pipe`), when the terminal is
  still cooked and a pre-translated `\r\n` would double up with the kernel's
  own `ONLCR`.
- **Connecting with no peer at all fails fast instead of hanging forever, via
  a plain-text connect-time handshake — see FORMAT.md §5.2.1 for the exact
  wire lines.** Before any of the fixes above, and still true without this
  one: running a `--pty` client with nothing on the other end of its fds
  (operator ran `bison-cli --pty` directly, not inside a `bison_server --pty`
  session) just hangs — the client's `OP_CONNECT` frame sits there forever,
  since there's no `stdio_server_transport` reader on the other end to
  decode it, and `rmi::client::connect()`'s `f.get()` has no timeout. This
  looks identical to every *other* bug this file has been about ("why is
  nothing happening"), except this time there's no fixing the framing —
  there's simply no peer.

  `stdio_client_transport::open()` now sends `START BISON/1.0\r\n` and
  blocks (5s default) for `BISON/1.0 OK\r\n` before proceeding; timing out
  throws instead of leaving `connect()` to hang. `stdio_server_transport`
  answers `START BISON/1.0` with `BISON/1.0 OK` automatically, watching for
  it continuously (not gated behind `accept()`) so it keeps working across
  reconnects. Implemented as a tap/gate on the passthrough stream rather
  than by extending the `BISON<...>` frame scanner's prefix-matching state
  machine to a second pattern — deliberately: that scanner has been the
  source of most of the subtle bugs fixed elsewhere in this file, and adding
  a second concurrent pattern to it was judged too risky for what these
  three fixed, short, literal lines actually need. The client *gates*
  (fully withholds passthrough until `BISON/1.0 OK` is seen, since letting
  it through would leak into `client_app`'s REPL input queue as a bogus
  typed command); the server only *taps* (forwards everything unchanged and
  separately watches a copy for `START BISON/1.0`, since its passthrough is
  purely for display, not input — see `stdio_server_transport`'s and
  `stdio_client_transport::open()`'s doc comments in
  `src/rmi/transport/stdio_transport.hpp` for the exact mechanics).

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
  `master_fd()` on the server side; wraps it in the `BISON<...>` framing.
  The client side of `stdio_transport` does not depend on this module at
  all (it wraps the client process's own already-connected stdio, no pty
  involved — see `src/app/client/client_app.cpp`).
- `src/app/server/server_app.cpp` — owns the `pty_process` instance in
  `--pty` mode, calls `start_pump()`, and blocks on `wait()` as its
  run-loop shutdown condition. `on_listening()`/`on_verbose_trace()` use
  `pty::write_raw()`/`to_crlf()` (`pty_write.hpp`) instead of plain
  `std::cout` when `FLAGS_pty` is set — see Design Decisions above.
- `src/app/client/client_app.cpp` — owns a `raw_mode_guard` and a
  `crlf_output_guard` on its own fd 0/1 in `--pty` mode, for the lifetime of
  the RMI session. This is the client side of the same `--pty` feature but
  does not depend on `pty_process` — see the termios note in Design
  Decisions above for why the two are separate abstractions. Constructs
  `crlf_output_guard` with a sink routed through
  `stdio_client_transport::send()`, not the default (direct-to-fd)
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
  flag-parsing/hook scaffolding. Also uses `pty::write_raw()`/`to_crlf()`
  for its two status messages, unconditionally (this function only ever
  runs in `--pty` mode, unlike `server_app`'s hooks).
