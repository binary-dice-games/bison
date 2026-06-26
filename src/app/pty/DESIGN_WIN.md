# Windows PTY Transport — Implementation Plan

## Context

The bison PTY transport multiplexes bison RMI DCS frames over a terminal channel
alongside normal shell I/O. Every PTY source file is currently guarded with
`#if defined(__linux__)`, making the feature unavailable on Windows. This document
describes the plan to add Windows support using the **ConPTY API** (available since
Windows 10 1809 / SDK 17763), keeping the Linux implementation untouched.

---

## Key API Mapping

| Linux | Windows |
|-------|---------|
| `forkpty()` | `CreatePseudoConsole()` + `CreateProcess()` |
| `termios` / `tcgetattr` / `tcsetattr` | `GetConsoleMode()` / `SetConsoleMode()` |
| `poll(fd, 1, 10ms)` | `PeekNamedPipe()` (10 ms sleep when empty) |
| `read(fd, buf, n)` | `ReadFile(HANDLE, buf, n, ...)` |
| `write(fd, buf, n)` | `WriteFile(HANDLE, buf, n, ...)` |
| `close(fd)` | `CloseHandle(HANDLE)` |
| `STDIN_FILENO` / `STDOUT_FILENO` | `GetStdHandle(STD_INPUT_HANDLE / OUTPUT_HANDLE)` |
| `waitpid()` | `WaitForSingleObject(hProcess)` |
| `isatty()` | `GetFileType(handle) == FILE_TYPE_CHAR` |

Default shell changes from `"bash"` to `"cmd.exe"` on Windows.

---

## Step 1 — Extend `dcs_framing.hpp` for Windows handles

**File:** `src/rmi/transport/dcs_framing.hpp`

**Changes:**
1. Remove the outer `#if defined(__linux__)` guard. The parser, base64, and protocol
   constants are pure STL — nothing in them is Linux-specific.
2. Replace `#include <unistd.h>` with a platform block:
   ```cpp
   #if defined(_WIN32)
   #  include <windows.h>
   #else
   #  include <unistd.h>
   #endif
   ```
3. Add a Windows overload of `write_all_fd` immediately after the Linux one:
   ```cpp
   #if defined(_WIN32)
   inline bool write_all_fd(HANDLE h, const void* data, size_t size) {
     const auto* p = static_cast<const CHAR*>(data);
     DWORD rem = static_cast<DWORD>(size), off = 0;
     while (rem > 0) {
       DWORD w = 0;
       if (!WriteFile(h, p + off, rem, &w, nullptr) || w == 0) return false;
       off += w; rem -= w;
     }
     return true;
   }
   #endif
   ```
4. `emit_dcs` and `emit_data` are already templated on `State`; they call
   `write_all_fd(fd, ...)` where `fd` is a function argument. No template signature
   change is needed — C++ overload resolution picks the right `write_all_fd` based on
   whether the caller passes `int` (Linux) or `HANDLE` (Windows).

---

## Step 2 — Extend transport headers to cover Windows

**Files:**
- `src/rmi/transport/pty_server_transport.hpp`
- `src/rmi/transport/pty_client_transport.hpp`

Change each file's outer guard from `#if defined(__linux__)` to
`#if defined(__linux__) || defined(_WIN32)`.

The public class interfaces are identical on both platforms — all platform-specific
details live in `struct impl` inside the `.cpp` files.

---

## Step 3 — Extend app scaffold headers and `server_app.hpp`

**Files:**
- `src/app/pty/pty_server_app.hpp`
- `src/app/pty/pty_client_app.hpp`
- `src/app/server/server_app.hpp`

For `pty_server_app.hpp` and `pty_client_app.hpp`: change outer guard to
`#if defined(__linux__) || defined(_WIN32)`.

For `pty_server_app.hpp`, also update `shell_command()`:
```cpp
std::string shell_command() const override {
#if defined(_WIN32)
  return "cmd.exe";
#else
  return "bash";
#endif
}
```

For `server_app.hpp`: the PTY virtual methods (`on_listening_pty`,
`on_pty_client_connected`, `on_pty_session_ended`, `shell_command`, `run_pty`) are
inside `#if defined(__linux__)` blocks at lines 92–123 and 174–185. Change both guards
to `#if defined(__linux__) || defined(_WIN32)`.

---

## Step 4 — Create `pty_server_transport_win.cpp`

**New file:** `src/rmi/transport/pty_server_transport_win.cpp`

Guarded with `#if defined(_WIN32)`. Mirrors `pty_server_transport.cpp` structurally.

### `pty_shared_state` (replaces `int master_fd` with two HANDLEs)

```cpp
struct pty_shared_state {
  HANDLE hRead  = INVALID_HANDLE_VALUE; // server reads shell output (pipe ← ConPTY)
  HANDLE hWrite = INVALID_HANDLE_VALUE; // server writes to shell   (pipe → ConPTY)

  std::mutex              write_mtx;
  std::mutex              read_mtx;
  std::condition_variable read_cv;
  std::queue<bison::buffer>                     inbox;
  std::unordered_map<uint64_t, partial_message> pending;

  std::atomic<bool>     closed{false};
  std::atomic<bool>     hello_seen{false};
  std::atomic<bool>     shell_running{false};
  std::atomic<bool>     stop_requested{false};
  std::atomic<uint64_t> next_msg_id{1};

  size_t max_chunk_bytes = 1536U;
  size_t max_frame_bytes = 8U * 1024U * 1024U;
  std::chrono::milliseconds reassembly_timeout{5000};
};
```

### `reader_loop` (replaces `poll` + `read`)

```cpp
void reader_loop(std::shared_ptr<pty_shared_state> state) {
  // flush_plain: WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), ...)
  // dcs_byte_parser: same as Linux

  while (!state->stop_requested.load()) {
    DWORD available = 0;
    if (!PeekNamedPipe(state->hRead, nullptr, 0, nullptr, &available, nullptr)) {
      state->shell_running.store(false);
      close_and_notify(*state); return;
    }
    if (available == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
      continue;
    }
    uint8_t c = 0; DWORD n = 0;
    if (!ReadFile(state->hRead, &c, 1, &n, nullptr) || n == 0) {
      state->shell_running.store(false);
      close_and_notify(*state); return;
    }
    parser.feed(c);
  }
  flush_plain(); close_and_notify(*state);
}
```

### `input_relay_loop` (replaces `poll` + `read(STDIN_FILENO)`)

```cpp
void input_relay_loop(std::shared_ptr<pty_shared_state> state) {
  HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
  char buf[256];
  while (!state->stop_requested.load()) {
    if (GetFileType(hIn) == FILE_TYPE_CHAR) {
      if (WaitForSingleObject(hIn, 100) != WAIT_OBJECT_0) continue;
    } else {
      DWORD avail = 0;
      if (!PeekNamedPipe(hIn, nullptr, 0, nullptr, &avail, nullptr)) break;
      if (avail == 0) { std::this_thread::sleep_for(std::chrono::milliseconds{10}); continue; }
    }
    DWORD n = 0;
    if (!ReadFile(hIn, buf, sizeof(buf), &n, nullptr) || n == 0) break;
    std::lock_guard<std::mutex> lk(state->write_mtx);
    write_all_fd(state->hWrite, buf, static_cast<size_t>(n));
  }
}
```

### `impl` struct

```cpp
struct pty_server_transport::impl {
  explicit impl(std::string shell)
      : shell_(std::move(shell)),
        state_(std::make_shared<pty_shared_state>()) {}

  std::string shell_;
  HPCON       hPC_{nullptr};
  HANDLE      hShellProcess_{INVALID_HANDLE_VALUE};
  HANDLE      hShellThread_ {INVALID_HANDLE_VALUE};
  HANDLE      hPipeIn_ {INVALID_HANDLE_VALUE};  // server-side write end → ConPTY
  HANDLE      hPipeOut_{INVALID_HANDLE_VALUE};  // server-side read end  ← ConPTY
  std::shared_ptr<pty_shared_state> state_;
  std::thread reader_thread_, input_relay_thread_;
  std::atomic<bool> started_{false}, accepted_{false};
};
```

### `start()` — ConPTY setup

```cpp
void pty_server_transport::start(bison::dynamic params) {
  (void)params;
  if (impl_->started_.exchange(true)) return;

  // 1. Create pipe pairs (non-inheritable in this process).
  HANDLE hInRead, hInWrite, hOutRead, hOutWrite;
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
  if (!CreatePipe(&hInRead, &hInWrite, &sa, 0) ||
      !CreatePipe(&hOutRead, &hOutWrite, &sa, 0)) {
    impl_->started_.store(false);
    throw std::runtime_error("pty_server_transport: CreatePipe failed");
  }

  // 2. Create the ConPTY, passing it the child-facing ends.
  COORD size{500, 50};
  HRESULT hr = CreatePseudoConsole(size, hInRead, hOutWrite, 0, &impl_->hPC_);
  CloseHandle(hInRead);   // now owned by ConPTY
  CloseHandle(hOutWrite); // now owned by ConPTY
  if (FAILED(hr)) {
    CloseHandle(hInWrite); CloseHandle(hOutRead);
    impl_->started_.store(false);
    throw std::runtime_error("pty_server_transport: CreatePseudoConsole failed");
  }

  // 3. Store the server-side ends.
  impl_->hPipeIn_         = hInWrite;
  impl_->hPipeOut_        = hOutRead;
  impl_->state_->hWrite   = hInWrite;
  impl_->state_->hRead    = hOutRead;

  // 4. Build STARTUPINFOEX with PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE.
  SIZE_T attrListSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attrListSize);
  auto attrBuf = std::vector<uint8_t>(attrListSize);
  auto* pAttrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
  if (!InitializeProcThreadAttributeList(pAttrList, 1, 0, &attrListSize)) {
    ClosePseudoConsole(impl_->hPC_); CloseHandle(hInWrite); CloseHandle(hOutRead);
    impl_->started_.store(false);
    throw std::runtime_error("pty_server_transport: InitializeProcThreadAttributeList failed");
  }
  UpdateProcThreadAttribute(pAttrList, 0,
      PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, impl_->hPC_, sizeof(impl_->hPC_),
      nullptr, nullptr);

  STARTUPINFOEXW si{};
  si.StartupInfo.cb  = sizeof(si);
  si.lpAttributeList = pAttrList;

  // 5. Launch the shell.
  std::wstring cmd(impl_->shell_.begin(), impl_->shell_.end());
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                           EXTENDED_STARTUPINFO_PRESENT,
                           nullptr, nullptr,
                           reinterpret_cast<LPSTARTUPINFOW>(&si), &pi);
  DeleteProcThreadAttributeList(pAttrList);
  if (!ok) {
    ClosePseudoConsole(impl_->hPC_); impl_->hPC_ = nullptr;
    CloseHandle(hInWrite); CloseHandle(hOutRead);
    impl_->started_.store(false);
    throw std::runtime_error("pty_server_transport: CreateProcess failed");
  }
  impl_->hShellProcess_ = pi.hProcess;
  impl_->hShellThread_  = pi.hThread;
  impl_->state_->shell_running.store(true);
  impl_->state_->stop_requested.store(false);

  // 6. Start reader and input-relay threads.
  impl_->reader_thread_      = std::thread(reader_loop,      impl_->state_);
  impl_->input_relay_thread_ = std::thread(input_relay_loop, impl_->state_);
}
```

### `accept()`

Identical logic to Linux. The `emit_dcs` call uses `st.hWrite` instead of `st.master_fd`:
```cpp
emit_dcs(st.hWrite, st,
    std::string{rmi::transport::dcs::kProtoVersion} + ";type=HELLO");
```

### `stop()`

```cpp
void pty_server_transport::stop() {
  if (!impl_->started_.load()) return;
  auto& st = *impl_->state_;

  // Signal shell to exit.
  if (st.hWrite != INVALID_HANDLE_VALUE) {
    std::lock_guard<std::mutex> lk(st.write_mtx);
    const char exit_cmd[] = "exit\r\n";
    write_all_fd(st.hWrite, exit_cmd, sizeof(exit_cmd) - 1);
  }

  st.stop_requested.store(true);
  st.shell_running.store(false);
  close_and_notify(st);

  if (impl_->hPC_) { ClosePseudoConsole(impl_->hPC_); impl_->hPC_ = nullptr; }
  if (impl_->hPipeIn_  != INVALID_HANDLE_VALUE) { CloseHandle(impl_->hPipeIn_);  impl_->hPipeIn_  = INVALID_HANDLE_VALUE; }
  if (impl_->hPipeOut_ != INVALID_HANDLE_VALUE) { CloseHandle(impl_->hPipeOut_); impl_->hPipeOut_ = INVALID_HANDLE_VALUE; }

  if (impl_->hShellProcess_ != INVALID_HANDLE_VALUE) {
    WaitForSingleObject(impl_->hShellProcess_, 5000);
    CloseHandle(impl_->hShellProcess_); impl_->hShellProcess_ = INVALID_HANDLE_VALUE;
    CloseHandle(impl_->hShellThread_);  impl_->hShellThread_  = INVALID_HANDLE_VALUE;
  }

  if (impl_->reader_thread_.joinable())       impl_->reader_thread_.detach();
  if (impl_->input_relay_thread_.joinable())  impl_->input_relay_thread_.detach();
}
```

### `pty_server_connection::send()`

Calls `emit_data(impl_->state->hWrite, *impl_->state, frame)`.

### `restart_session()`, `is_shell_running()`, `wait_until_closed()`

Identical to Linux — they only touch mutex/atomic fields in `pty_shared_state`,
which are the same on both platforms.

---

## Step 5 — Create `pty_client_transport_win.cpp`

**New file:** `src/rmi/transport/pty_client_transport_win.cpp`

Guarded with `#if defined(_WIN32)`. Mirrors `pty_client_transport.cpp`.

### `client_state` (replaces `int read_fd / write_fd`)

```cpp
struct client_state {
  HANDLE read_h  = INVALID_HANDLE_VALUE; // STD_INPUT_HANDLE
  HANDLE write_h = INVALID_HANDLE_VALUE; // STD_OUTPUT_HANDLE

  std::mutex              write_mtx;
  std::mutex              read_mtx;
  std::condition_variable read_cv;
  std::queue<bison::buffer>                     inbox;
  std::unordered_map<uint64_t, partial_message> pending;

  std::atomic<bool>     closed{false};
  std::atomic<bool>     stop_requested{false};
  std::atomic<bool>     hello_seen{false};
  std::atomic<uint64_t> next_msg_id{1};

  size_t max_chunk_bytes = 1536U;
  size_t max_frame_bytes = 8U * 1024U * 1024U;
  std::chrono::milliseconds reassembly_timeout{5000};
  std::chrono::milliseconds handshake_timeout{300000};
};
```

### `client_reader_loop`

Same `PeekNamedPipe` / `WaitForSingleObject` + `ReadFile` pattern as the server's
input relay, reading `state->read_h` one byte at a time. Non-DCS bytes are silently
discarded (no `on_plain` callback), identical to the Linux client.

### `open()`

No `termios` raw-mode block — stdin is a pipe inside ConPTY. The handshake is
otherwise identical to Linux:

```cpp
void pty_client_transport::open(bison::dynamic params) {
  auto& st = *impl_->state;
  st.read_h  = GetStdHandle(STD_INPUT_HANDLE);
  st.write_h = GetStdHandle(STD_OUTPUT_HANDLE);
  st.closed.store(false);
  st.stop_requested.store(false);
  st.hello_seen.store(false);

  if (const auto* f = params.findField("handshake_timeout_ms"_key);
      f != nullptr && f->is<int32_t>())
    st.handshake_timeout =
        std::chrono::milliseconds{std::max(0, f->as<int32_t>())};

  if (!impl_->reader.joinable())
    impl_->reader = std::thread(client_reader_loop, impl_->state);

  emit_dcs(st.write_h, st,
      std::string{rmi::transport::dcs::kProtoVersion} + ";type=HELLO");

  {
    std::unique_lock<std::mutex> lk(st.read_mtx);
    const bool ok = st.read_cv.wait_for(lk, st.handshake_timeout, [&st] {
      return st.hello_seen.load() || st.closed.load();
    });
    if (!ok || !st.hello_seen.load())
      throw std::runtime_error("pty_client_transport::open: handshake timeout");
  }
  impl_->opened = true;
}
```

### `send()` / `receive()` / `shutdown()`

Same logic as Linux, with `st.write_h` in place of `st.write_fd` and
`st.read_h` in place of `st.read_fd`. `shutdown()` omits the `tcsetattr` restore.

---

## Step 6 — Extend `server_app.cpp` and app `.cpp` files

### `src/app/server/server_app.cpp`

Four guarded sections currently use `#if defined(__linux__)`:
- `#include "src/rmi/transport/pty_server_transport.hpp"` near the top
- The `bridged_server` PTY helper struct
- `run_pty()` implementation
- The `FLAGS_pty` branch in `run()`

Change all four guards to `#if defined(__linux__) || defined(_WIN32)`.

The `run_pty()` body calls `pty_server_transport{shell_command()}` — this works on
Windows because `shell_command()` now returns `"cmd.exe"` when `_WIN32` is defined.

### `src/app/pty/pty_server_app.cpp` and `pty_client_app.cpp`

Change outer guard to `#if defined(__linux__) || defined(_WIN32)`. Bodies are
platform-agnostic (call into `server_app::run_pty()` / `client_app::run_with_transport()`).

### `src/app/pty/pty_c.cpp` and `include/pty_c.h`

Change all `#if defined(__linux__)` guards to `#if defined(__linux__) || defined(_WIN32)`.
The C++ wrapper classes delegate to `pty_server_app` / `pty_client_app` which now
work on Windows. The fallback `return RMI_ERR_INVALID_STATE` branches remain for
other platforms (macOS, etc.).

---

## Step 7 — CMake wiring

**File:** `CMakeLists.txt`

Replace the four unconditional PTY source entries (currently listed together in the
`target_sources(bison PRIVATE ...)` block at lines 93–100) with
platform-conditional blocks:

```cmake
# PTY transport — platform-specific implementations
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(bison PRIVATE
    src/rmi/transport/pty_server_transport.cpp
    src/rmi/transport/pty_client_transport.cpp
    src/app/pty/pty_server_app.cpp
    src/app/pty/pty_client_app.cpp
  )
  target_link_libraries(bison_abi PRIVATE util)
elseif(WIN32)
  target_sources(bison PRIVATE
    src/rmi/transport/pty_server_transport_win.cpp
    src/rmi/transport/pty_client_transport_win.cpp
    src/app/pty/pty_server_app.cpp
    src/app/pty/pty_client_app.cpp
  )
  # ConPTY requires Windows 10 1809 (SDK 17763 / _WIN32_WINNT=0x0A00).
  # Override per-source only to avoid raising the global _WIN32_WINNT that
  # asio_headers targets at 0x0601.
  set_source_files_properties(
    src/rmi/transport/pty_server_transport_win.cpp
    src/rmi/transport/pty_client_transport_win.cpp
    PROPERTIES COMPILE_DEFINITIONS "_WIN32_WINNT=0x0A00"
  )
endif()
```

Note: the existing `target_link_libraries(bison_abi PRIVATE util)` at line 132 is
already inside `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` — move it into the new
`if(Linux)` block above if it is not already there. `CreatePseudoConsole` lives in
`kernel32.dll`, which MSVC links implicitly — no extra `target_link_libraries` needed
on Windows.

---

## Step 8 — Update `DESIGN.md`

**File:** `src/app/pty/DESIGN.md`

- **Section 1** "Purpose and Scope": change "Linux/POSIX only. All files are guarded
  with `#if defined(__linux__)`." to "Linux and Windows. All files are guarded with
  `#if defined(__linux__) || defined(_WIN32)`."
- **Section 7** "Constraints and Invariants": replace the Linux-only bullet with:
  - Linux: `forkpty`, `termios`, `poll` — handled in `pty_server_transport.cpp` /
    `pty_client_transport.cpp`.
  - Windows: ConPTY (`CreatePseudoConsole`, `CreateProcess`), `PeekNamedPipe`,
    `ReadFile`/`WriteFile` — handled in `*_win.cpp`. Requires Windows 10 1809+
    (SDK 17763 / `_WIN32_WINNT=0x0A00`).
- **Add "Windows Implementation Notes" subsection** covering:
  - ConPTY creates two anonymous pipes; the server-facing ends (`hRead`, `hWrite`)
    replace the single `master_fd` used on Linux.
  - Polling uses `PeekNamedPipe` with a 10 ms sleep instead of `poll()`.
  - Console stdin input uses `WaitForSingleObject` (100 ms timeout); pipe stdin uses
    `PeekNamedPipe`.
  - `_WIN32_WINNT=0x0A00` is set per-source in CMake to avoid raising the global
    version used by ASIO.
  - `write_all_fd(HANDLE)` overload in `dcs_framing.hpp` allows the shared
    `emit_dcs` / `emit_data` templates to work on Windows without changes.

---

## Verification

1. **Linux regression**: `cmake --build && ctest` on Linux — all existing tests pass
   unchanged.
2. **Windows compile**: `cmake --build` on Windows (SDK ≥ 17763) — no errors or
   warnings on the new files.
3. **DCS round-trip unit test**: `CreatePipe` pair → `write_all_fd(HANDLE, ...)` on
   write end → `ReadFile` on read end → bytes match. Validates the new
   `dcs_framing.hpp` overload in isolation.
4. **End-to-end smoke test**: run `pty_server_example` on Windows; in a second
   process run `pty_client_example`. Confirm the bison session establishes, a method
   call returns the expected value, and `cmd.exe` output is relayed to stdout.
5. **Cleanup**: after the client disconnects, `WaitForSingleObject(hShellProcess_,
   5000)` returns `WAIT_OBJECT_0` (not `WAIT_TIMEOUT`) and no handle leaks occur.

---

## Gotchas

| Risk | Mitigation |
|------|-----------|
| ConPTY not available on SDK < 17763 | `set_source_files_properties` sets `_WIN32_WINNT=0x0A00`; build fails clearly if SDK is too old |
| Child inherits server pipe ends | `SECURITY_ATTRIBUTES.bInheritHandle = FALSE`; close child-facing pipe ends after `CreatePseudoConsole` |
| `CreateProcessW` requires `wstring` | Widen `shell_` with `std::wstring(shell_.begin(), shell_.end())` — sufficient for `cmd.exe` / ASCII paths |
| `_WIN32_WINNT` conflict with ASIO | Override per-source only; global stays at `0x0601` |
| VT sequences in ConPTY output | `dcs_byte_parser` `on_plain` handler passes them to `flush_plain` → stdout; same as Linux |
| `PeekNamedPipe` busy-poll CPU | 10 ms sleep keeps CPU near-zero at rest; same cadence as Linux `poll` 10 ms timeout |
