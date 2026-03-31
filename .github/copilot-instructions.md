# Bison Coding Style Guide for Copilot

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

## Python Binding Style

- Use 4-space indentation and type hints in signatures where practical.
- Use descriptive module/class/function docstrings.
- Keep naming Pythonic: snake_case functions, PascalCase classes.
- Raise typed exceptions with clear context messages.

## C# Binding Style

- Keep concise, readable control flow.
- Use PascalCase for types/methods, camelCase for locals/parameters.
- Prefer XML documentation comments on public APIs.
- Keep nullable-awareness enabled and avoid null-forgiving unless required.

## Java Binding Style

- Use standard Java formatting with clear Javadoc on public APIs.
- Prefer try-with-resources for lifecycle-managed types.
- Use descriptive method names and explicit error context.

## Documentation and Comments

- Add comments when they explain intent, protocol constraints, ownership, or threading.
- Avoid redundant comments that only restate the code.
- Keep public API docs precise and implementation docs brief.

## Copilot Behavioral Rules for This Repo

- Do not introduce broad stylistic rewrites.
- Do not change naming conventions in existing APIs.
- When adding new C++ files, mirror the RMI/core style in nearby files.
- When adding bindings/tests, mirror style from sibling binding/test files.
- If style is ambiguous, prefer consistency with adjacent code over generic defaults.
