# Documentation & Project Files Maintenance

## README.md — keep it concise

`README.md` is the first file agents read each conversation. Its purpose is to give a quick project overview, core build commands, a quick-start snippet, and a summary of concepts — then link out to `docs/` for details. **Do not inline long how-to content into README.md.**

Rules:
- Keep `README.md` under ~200 lines. Prose should be tight; no padded explanations.
- Detailed setup instructions belong in `docs/building.md`.
- Example run instructions belong in `docs/examples.md`.
- Language binding setup and usage belong in `docs/bindings.md`.
- Benchmark details and optimization notes belong in `docs/performance.md`.
- Wire format specification lives in `FORMAT.md` (already referenced from README).
- When you add or change a feature, update the relevant section in `README.md` (or the relevant `docs/` file) to reflect the change. Do not leave stale content.
- If you add a new `docs/` file, add a row for it in the "Further Documentation" table in `README.md`.

## FORMAT.md — wire format spec

Update `FORMAT.md` whenever the binary wire format, RMI envelope schema, or transport framing changes. Keep it precise and implementation-agnostic.

## Code documentation

Maintain clear, concise doc comments in code, especially for public APIs, classes, and module-level contracts. Prefer brief Doxygen comments that explain intent, parameters, return values, and failure modes.

## DESIGN.md — directory architecture docs

Some directories contain a `DESIGN.md` file that describes the architecture, key abstractions, and design decisions for the code in that directory. When working in a directory:
- Read `DESIGN.md` if it exists before making changes, to understand the intended design.
- After making changes that affect the architecture, public API surface, or key design decisions, update `DESIGN.md` to reflect those changes. Keep it accurate and concise — it should explain the *why* behind the structure, not restate what the code already says.
- Do not create a `DESIGN.md` unless asked to. Only maintain existing ones.

## Startup reading

At the start of every new conversation, always read `README.md` to understand the project's structure and goals. Read `FORMAT.md` only when working on serialization, RMI, or transport. When task-specific details are needed, read the relevant `docs/` file and the in-code documentation.

## Tests

Always write automated tests validating any new or modified behavior. Maintain and update tests alongside code changes; tests should be concise, deterministic, and focused on public behavior.

# Bison Coding Style Guide for Claude Code Assist

Use this guide when generating or editing code in this repository.

## General

- Match the style already used in nearby files first.
- Keep changes minimal and focused; do not reformat unrelated code.
- Preserve existing public API names and behavior unless explicitly requested.
- Prefer readable, explicit code over clever shortcuts.
- Use ASCII by default.

## C++ Style (Primary)

### Formatting

- Follow `.clang-format` exactly.
- Use 2-space indentation, never tabs.
- Keep line length around 80 columns.
- Use attached braces (`if (...) {`, `class X {`, `namespace N {`).
- Use one space before control statement parentheses (`if (...)`, `for (...)`).
- Use pointer alignment on the left (`Type* ptr`).
- Let includes be sorted by formatter rules.

### File Structure

- Start files with the MIT license header used in this repo.
- Add `@file` and `@brief` Doxygen comments for public headers and key sources.
- Use section dividers for readability in larger files (for example: `// ── Section ──`).

### Includes

- In headers/sources, keep project includes grouped before standard library includes.
- Prefer explicit includes over relying on transitive includes.

### Naming

- Keep namespace style as `bdg::bison::...` and close with a namespace comment.
- Follow existing naming in each subsystem:
  - RMI code uses snake_case types/functions (`memory_server_transport`, `send_response`).
  - Legacy core APIs may use existing camelCase symbols; do not rename them.
- Use trailing underscores for private member fields (`running_`, `mtx_`).
- Use descriptive local names (`payload_bytes`, `request_id`, `workers_mutex_`).

### API and Error Handling

- Prefer clear contract comments on public methods (`@param`, `@return`, failure behavior).
- Use `std::runtime_error` (and derived errors) for C++ error reporting where appropriate.
- At C ABI boundaries, catch all exceptions and convert to error codes/null handles.
- For optional results, prefer `std::optional` over sentinel values.

### Concurrency

**Prefer `bison::synchronized<T>` over raw `std::mutex` for all shared state.**
`synchronized<T>` wraps a value and makes it inaccessible without explicitly
calling `.rlock()` (shared read) or `.wlock()` (exclusive write), so the type
system enforces synchronization rather than relying on comments or convention.
Avoid raw `std::mutex`, `std::lock_guard`, and `std::unique_lock` except at the
lowest implementation level (e.g. inside custom data structures).

## Platform Support

Bison builds on three platform configurations: Linux, MSYS2, and native
Windows (MSVC or mingw64, *not* MSYS2/Cygwin). MSYS2 provides the same
POSIX layer as Linux (`forkpty()`, `termios`, `unistd.h`, etc.), so Linux
and MSYS2 always share one implementation. `CMakeLists.txt` distinguishes
"genuinely native Windows" from MSYS2/Cygwin with the condition
`WIN32 AND NOT MSYS AND NOT CYGWIN`.

**Rule: when a file needs a real, different implementation on native
Windows than on Linux/MSYS2, split it into a `_posix.cpp`/`_win.cpp` pair
sharing one header, selected in `CMakeLists.txt` via
`if(WIN32 AND NOT MSYS AND NOT CYGWIN) ... else() ... endif()`.** This is
not limited to any one subsystem — apply it anywhere a genuine OS-level
difference shows up: process/terminal spawning (`forkpty()` vs. ConPTY),
fd/socket duplication (`dup()`/`_dup()`, or `WSADuplicateSocket` for
Winsock `SOCKET`s, which aren't plain fds), console raw-mode APIs
(`termios` vs. `GetConsoleMode`/`SetConsoleMode`), debugger-attachment
detection (`/proc/self/status` vs. `IsDebuggerPresent()`), and similar
cases. Do not add a separate platform-suffixed file for a difference that
MSYS2 already resolves the same way Linux does (MSYS2 is never the
"other" side of one of these splits). Prefer a pImpl / opaque-state header
(forward-declare a `struct foo_state;`, defined differently per `.cpp`) so
the shared header needs no platform-specific members, matching
`src/pty/raw_mode_guard.hpp`.

Avoid conditional compilation (`#ifdef`, `#if defined(...)`) to branch on
OS behavior *within* a shared file. If a genuine platform difference is
narrow enough to stay inline (for example native Linux's `eventfd` vs.
MSYS2's lack of it, both still within the POSIX target), keep it as a
small, narrowly-scoped `#if defined(__linux__)` guard rather than a full
file split. Reserve the full split for cases where the two
implementations use different OS subsystems entirely (`forkpty()` vs.
ConPTY, `termios` vs. console-mode APIs, etc.) — that's not a narrow
delta, so a single `#ifdef`-branched file would be worse than two clean
ones.

**Some subsystems are deliberately Linux/MSYS2-only and are excluded from
native Windows builds entirely**, rather than split: `src/pty/pty_process.cpp`
and `src/console/console_process.cpp` (forkpty()-based process spawning has
no native Windows equivalent; ConPTY-based spawning lives in `src/term`
instead) and the `util` system library they link against. `CMakeLists.txt`
gates their sources and link behind `NOT (WIN32 AND NOT MSYS AND NOT CYGWIN)`
and defines `BISON_NATIVE_WINDOWS` on the native-Windows build so
`server_app.cpp` can report a runtime error for `--transport=pty`/
`--transport=console` there instead of failing to compile. When adding a
new file, decide up front whether it belongs in this "excluded" category
or the "real per-platform implementation" category above — don't leave it
unconditionally compiled and let native Windows fail on it later.

`src/term` (`--transport=term`) is the reference example of the file-split
pattern: `src/term/terminal_win.cpp` implements terminal spawning via
ConPTY (`CreatePseudoConsole`) for native Windows, alongside
`src/term/terminal_posix.cpp` (`forkpty()`, used on Linux and MSYS2).
`src/rmi/transport/term_transport.cpp` itself is platform-independent
(built on libuv) and compiles unconditionally, same as the rest of
`src/rmi/transport` — transport code should stay portable by delegating
any OS-specific pieces (e.g. fd duplication, socket duplication) to a
`_posix.cpp`/`_win.cpp` helper pair rather than including a POSIX header
directly.

## Testing Style (GoogleTest)

- Use `TEST` / `TEST_F` with descriptive suite and test names.
- Prefer `ASSERT_*` for preconditions and `EXPECT_*` for subsequent checks.
- Group tests with clear section banners in larger test files.
- Keep tests deterministic and avoid timing-sensitive flakiness where possible.

## Python/C#/Java Binding Style

- Maintain consistent formatting and naming (snake_case for Python, PascalCase for C#/Java).
- Ensure public APIs have clear documentation (docstrings or XML comments).

## Claude Code Assist Behavioral Rules for This Repo

- Do not introduce broad stylistic rewrites.
- Do not change naming conventions in existing APIs.
- When adding new C++ files, mirror the RMI/core style in nearby files.
- When adding bindings/tests, mirror style from sibling binding/test files.
- If style is ambiguous, prefer consistency with adjacent code over generic defaults.