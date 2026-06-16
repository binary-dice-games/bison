# Documentation & Project Files Maintenance

- Keep `FORMAT.md` and `README.md` updated: the Claude agent must update these files to reflect changes it makes. Keep them concise — include only information necessary to understand project layout, build/test commands, and conventions. Avoid adding excessive detail that bloats the agent context.
- Keep code documentation updated: maintain clear, concise documentation in code, especially for public APIs, classes, and module-level contracts. Prefer brief doc comments that explain intent, parameters, return values, and failure modes.
- Startup reading: at the start of every new conversation, always read `FORMAT.md` and `README.md` to understand the project's structure and goals. When task-specific details are needed, read the in-code documentation and public API comments to obtain additional context.
- Tests: always write automated tests validating any new or modified behavior. Maintain and update tests alongside code changes to ensure continued correctness; tests should be concise, deterministic, and focused on public behavior.

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

- Use standard primitives (`std::mutex`, `std::lock_guard`, `std::shared_mutex`, atomics).
- Keep lock scope tight.
- Use condition variables with explicit shutdown/stop flags.

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