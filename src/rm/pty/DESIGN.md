# PTY Abstraction Layer Design

## 1. Purpose and Scope

`src/rm/pty` provides two base classes — `pty_server` and `pty_client` — for
building applications that multiplex an out-of-band byte channel alongside a
real interactive console.

- **`pty_server`** spawns a shell (bash, cmd, PowerShell, …) inside a real
  pseudoterminal so the user can type commands interactively. It intercepts DCS
  escape sequences injected by the remote side and lets subclasses process them.
  It can also inject DCS sequences that the remote side will receive.
- **`pty_client`** reads the process's own stdin/stdout (the PTY slave side,
  typically reached via SSH or a direct PTY). It intercepts DCS sequences sent
  by the server and lets subclasses process them. It can also send DCS sequences
  back to the server.

Both sides can **send and receive DCS frames at any time**; the channel is fully
bidirectional. Normal terminal I/O is unaffected.

This directory owns:

- `pty_server.hpp` / `pty_server.cpp` (Linux: `pty_server_linux.cpp`,
  Windows: `pty_server_win.cpp`) — `pty_server` base class.
- `pty_client.hpp` / `pty_client.cpp` — `pty_client` base class (cross-platform
  via libuv).

This directory does **not** own:

- DCS framing helpers — `src/rmi/transport/dcs_framing.hpp`.
- RMI transport adapters — `src/rmi/transport/pty_*_transport.*`.
- Application scaffolds — `src/app/pty/`.

## 2. Why a Real PTY Is Required

Shells and interactive programs call `isatty(stdin)` to detect a terminal. When
the answer is false — which is always the case when the process is connected via
ordinary pipes — they disable readline editing, colour output, job control, and
interactive prompts permanently.

`pty_server` must create the subprocess inside a **real PTY**:

- **Linux:** `forkpty()` — allocates a master/slave PTY pair and forks the
  child with its stdin/stdout/stderr connected to the slave. `isatty(0)` in the
  child returns true.
- **Windows:** `CreatePseudoConsole()` + `CreateProcess()` — ConPTY allocates
  the PTY and the child process sees a real console. Requires Windows 10 1809+
  (SDK 17763).

`uv_spawn()` with pipes cannot be used here; it produces non-TTY stdin/stdout
and breaks interactive shells.

## 3. Design

Both classes are designed to be **subclassed**. The base class owns the PTY
lifecycle and the DCS byte-level parsing (via `dcs_byte_parser` from
`dcs_framing.hpp`). The subclass overrides virtual methods to add behaviour.

```
          ┌──────────────────────────┐
          │       pty_server         │  ← base class, owns PTY + DCS parser
          │  - open(command)         │
          │  - close()               │
          │  - send(body)            │  ← inject DCS to client
          │  - write(data, n)        │  ← relay keystrokes to shell
          │ [on_output(data,n)]      │  ← shell output to user (virtual)
          │ [on_received(body)]      │  ← DCS from client (virtual)
          │ [on_shell_exit()]        │  ← shell exited (virtual)
          └──────────┬───────────────┘
                     │ inherits
          ┌──────────┴───────────────┐
          │  my_server : pty_server  │  ← user-defined subclass
          │  on_received(body) {     │
          │    // decode body,       │
          │    // do something,      │
          │    send(reply_body);     │
          │  }                       │
          └──────────────────────────┘

          ┌──────────────────────────┐
          │       pty_client         │  ← base class, owns DCS parser on stdin
          │  - open()                │
          │  - close()               │
          │  - send(body)            │  ← send DCS to server via stdout
          │  - write(data, n)        │  ← write raw bytes to stdout
          │ [on_received(body)]      │  ← DCS from server (virtual)
          │ [on_input(data,n)]       │  ← non-DCS bytes on stdin (virtual)
          │ [on_closed()]            │  ← stdin closed (virtual)
          └──────────┬───────────────┘
                     │ inherits
          ┌──────────┴───────────────┐
          │  my_client : pty_client  │  ← user-defined subclass
          │  on_received(body) {     │
          │    // decode body,       │
          │    // do something,      │
          │    send(reply_body);     │
          │  }                       │
          └──────────────────────────┘
```

## 4. `pty_server` API

```cpp
namespace bdg::bison::rm::pty {

class pty_server {
 public:
  virtual ~pty_server();

  // ── Lifecycle ──────────────────────────────────────────────────────────────

  /**
   * @brief Spawn @p command in a real PTY and start the I/O loop.
   *
   * Linux: forkpty() — child has isatty(0) == true.
   * Windows: CreatePseudoConsole() + CreateProcess().
   *
   * Switches the caller's terminal to raw/no-echo mode so keystrokes are
   * forwarded to the shell unmodified.
   *
   * @throws std::runtime_error on failure.
   */
  void open(const std::string& command = default_shell());

  /**
   * @brief Terminate the shell and shut down the I/O loop.
   *
   * Restores the caller's terminal mode. Safe to call if open() was never
   * called or already called close(). The destructor calls this automatically.
   */
  void close();

  /** @brief Returns true while the shell subprocess is running. */
  bool is_open() const;

  // ── Sending ────────────────────────────────────────────────────────────────

  /**
   * @brief Send a DCS escape sequence to the client.
   *
   * Wraps @p body as `ESC P <body> ESC \` and writes it to the PTY master.
   * The client's on_received() will be called with @p body.
   * Thread-safe.
   */
  void send(const std::string& body);

  /**
   * @brief Write raw bytes to the PTY master (user keystroke relay).
   *
   * Sends @p data to the shell's stdin as if the user typed it.
   * Thread-safe.
   */
  void write(const uint8_t* data, size_t n);

 protected:
  // ── Receiving ──────────────────────────────────────────────────────────────

  /**
   * @brief Called when the shell produces non-DCS output.
   *
   * Default implementation writes @p data to the caller's stdout so the user
   * sees the shell. Override to capture or transform the output.
   *
   * Called on the I/O loop thread — do not block.
   */
  virtual void on_output(const uint8_t* data, size_t n);

  /**
   * @brief Called when a complete DCS frame arrives from the client.
   *
   * @p body is the raw content between `ESC P` and `ESC \`.
   * Override to process the frame. Call send() to reply.
   *
   * Called on the I/O loop thread — do not block.
   */
  virtual void on_received(const std::string& body) = 0;

  /**
   * @brief Called when the shell subprocess exits.
   * Default is a no-op.
   */
  virtual void on_shell_exit() {}

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

/** @brief Returns "bash" on Linux, "cmd.exe" on Windows. */
std::string default_shell();

} // namespace bdg::bison::rm::pty
```

## 5. `pty_client` API

```cpp
namespace bdg::bison::rm::pty {

class pty_client {
 public:
  virtual ~pty_client();

  // ── Lifecycle ──────────────────────────────────────────────────────────────

  /**
   * @brief Start reading stdin and the I/O loop.
   *
   * Uses uv_guess_handle(0) to detect whether stdin is a TTY (uv_tty_t,
   * opened in raw mode) or a pipe (uv_pipe_t). Both work transparently.
   *
   * @param raw_mode  If true and stdin is a TTY, switch to raw mode so DCS
   *                  frames are not buffered or echoed. Restored on close().
   */
  void open(bool raw_mode = true);

  /**
   * @brief Stop the I/O loop and restore terminal mode if changed.
   * The destructor calls this automatically.
   */
  void close();

  // ── Sending ────────────────────────────────────────────────────────────────

  /**
   * @brief Send a DCS escape sequence to the server.
   *
   * Wraps @p body as `ESC P <body> ESC \` and writes it to stdout.
   * The server's on_received() will be called with @p body.
   * Thread-safe.
   */
  void send(const std::string& body);

  /**
   * @brief Write raw bytes to stdout.
   *
   * Use this to pass non-DCS content back toward the server or local terminal.
   * Thread-safe.
   */
  void write(const uint8_t* data, size_t n);

 protected:
  // ── Receiving ──────────────────────────────────────────────────────────────

  /**
   * @brief Called when a complete DCS frame arrives from the server.
   *
   * @p body is the raw content between `ESC P` and `ESC \`.
   * Override to process the frame. Call send() to reply.
   *
   * Called on the I/O loop thread — do not block.
   */
  virtual void on_received(const std::string& body) = 0;

  /**
   * @brief Called for each non-DCS byte arriving on stdin.
   *
   * Default is a no-op. Override if the client needs to inspect or relay
   * terminal content that is not part of the DCS channel.
   *
   * Called on the I/O loop thread — do not block.
   */
  virtual void on_input(const uint8_t* data, size_t n) {}

  /**
   * @brief Called when stdin closes.
   * Default is a no-op.
   */
  virtual void on_closed() {}

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace bdg::bison::rm::pty
```

## 6. Bidirectional DCS Channel

Both `pty_server::send` and `pty_client::send` can be called at any time after
`open()`. Neither side needs to wait for the other before sending. The channel
is full-duplex:

```
pty_server::send(body)  ──► PTY master ──► PTY slave ──► pty_client::on_received(body)
pty_server::on_received(body) ◄── PTY master ◄── PTY slave ◄── pty_client::send(body)
```

DCS frames flow through the same byte stream as normal terminal I/O.
`dcs_byte_parser` separates them on each side: DCS bodies go to `on_received`,
everything else goes to `on_output` / `on_input`.

## 7. DCS Encoding

Frames are encoded using the ANSI DCS escape sequence:

```
ESC P <body> ESC \
      └─────┘
       passed to on_received()
```

`send(body)` wraps and emits the frame. `dcs_byte_parser` (from
`src/rmi/transport/dcs_framing.hpp`) handles the byte-level state machine on
the receive side. Both `pty_server` and `pty_client` use it internally; callers
only deal with the raw `body` string.

## 8. Data Flow

### Full SSH relay

```
Local machine                                    Remote machine
────────────────────────────────────             ──────────────────────────────
my_server : pty_server                           my_client : pty_client
  open("bash")                                     open()
       │                                                 │
       ▼                                                 ▼
  [PTY master]          SSH channel              [stdin / stdout]
       │                                                 │
  ┌────┴────────────────────────────────────────────────┴────┐
  │                  bidirectional byte stream               │
  └──────────────────────────────────────────────────────────┘
       │                                                 │
  ┌────┴────┐                                      ┌─────┴────┐
  │on_output│ → caller stdout (user sees terminal) │on_input  │ (non-DCS)
  │on_received│ ← send() from client               │on_received│ ← send() from server
  │send()   │ → on_received() on client            │send()    │ → on_received() on server
  └─────────┘                                      └──────────┘
```

### No active client (steady state)

Normal terminal relay works regardless of whether any DCS frames are being
exchanged:

```
user kbd → write() → PTY master → PTY slave (bash)
bash output → PTY master → dcs_byte_parser
  ├─ non-DCS → on_output() → stdout (user sees terminal)
  └─ DCS     → on_received()
```

## 9. Platform Implementation

### PTY creation

PTY allocation is platform-specific and is split into two source files (no
`#ifdef` in shared headers):

| File | Platform | Key calls |
|---|---|---|
| `pty_server_linux.cpp` | Linux | `forkpty()`, `tcgetattr/tcsetattr`, `waitpid()` |
| `pty_server_win.cpp` | Windows | `CreatePseudoConsole()`, `CreateProcess()` |

### libuv for I/O

Once `forkpty` returns a master fd, or `CreatePseudoConsole` yields pipe handles,
the platform-specific code wraps them in libuv via `uv_pipe_open()`. All
subsequent reading, write queuing, and shutdown use the libuv event loop:

- `uv_read_start()` feeds bytes into `dcs_byte_parser`.
- Writes (from `send()` and `write()`) are enqueued under a mutex and issued on
  the loop thread via `uv_async_t` → `uv_write()`.
- `stop_async` signals the loop thread to close all handles and return from
  `uv_run`.

`pty_client.cpp` is a single cross-platform file: `uv_guess_handle(0)` picks
`uv_tty_t` (TTY) or `uv_pipe_t` (pipe) automatically.

### Platform mapping

| Concept | Linux | Windows |
|---|---|---|
| PTY creation | `forkpty()` → `int master_fd` | `CreatePseudoConsole()` → `HANDLE hRead, hWrite` |
| libuv wrapping | `uv_pipe_open(master_fd)` | `uv_pipe_open(hRead/hWrite)` |
| Process wait | `waitpid()` | `WaitForSingleObject(hProcess)` |
| Terminal raw mode (server) | `tcsetattr(TCSANOW, ...)` | `SetConsoleMode(...)` |
| Terminal raw mode (client TTY) | `uv_tty_set_mode(UV_TTY_MODE_RAW)` | Same libuv call |
| Terminal size change | `ioctl(TIOCSWINSZ)` | `ResizePseudoConsole()` |

**Windows notes:**
- ConPTY requires SDK 17763 (`_WIN32_WINNT=0x0A00`); set per-source in CMake.
- Child-facing pipe ends (`hInRead`, `hOutWrite`) must be closed immediately
  after `CreatePseudoConsole` — ConPTY takes ownership.

## 10. Thread Model

Both classes run a single background **I/O loop thread** that owns the libuv
event loop:

- All libuv callbacks execute on the loop thread.
- `on_output`, `on_received`, `on_input`, `on_shell_exit`, and `on_closed` are
  all called on the loop thread. Overrides must not block and must not hold any
  mutex that the loop thread also acquires when dispatching writes.
- `send()` and `write()` are called from any thread. They enqueue data under a
  mutex and signal a `uv_async_t`; the actual `uv_write()` happens on the loop
  thread. It is safe to call `send()` from inside `on_received()`.
- `open()` and `close()` are called from the user thread. `close()` signals
  `stop_async`, joins the loop thread, and restores terminal state.

## 11. CMake Wiring

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(bison PRIVATE
    src/rm/pty/pty_server_linux.cpp
    src/rm/pty/pty_client.cpp
  )
  target_link_libraries(bison PRIVATE util)   # forkpty
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

## 12. Constraints and Invariants

- `pty_server` **must** use `forkpty` / `CreatePseudoConsole`. `uv_spawn` with
  pipes gives the child non-TTY stdin and breaks interactive shells.
- The loop thread is the sole owner of all libuv handles. Never access handles
  from another thread; use `uv_async_send` to wake the loop.
- `close()` is idempotent and safe to call if `open()` was never called.
  The destructor calls `close()`.
- Closing the PTY master fd/handle without calling `close()` first sends EOF to
  the child on Linux and terminates the ConPTY session on Windows — always go
  through `close()`.
- On Linux, the caller's terminal is put into raw mode by `open()` and restored
  by `close()`. Install a SIGTERM/SIGINT handler that calls `close()` to avoid
  leaving the terminal in raw mode on abnormal exit.
- On Windows, the child-facing pipe ends must be closed by the server process
  immediately after `CreatePseudoConsole` returns.
