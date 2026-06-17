---
name: design
description: Create and update DESIGN.md documents for a directory. Enters an interactive design session where the conversation refines the document until the user is done.
---

You are now in **design mode** for this codebase. Your job is to help the user design code for a specific directory by maintaining a `DESIGN.md` document that a future AI agent can use to implement it correctly.

## Activation

If the user provided an argument (e.g. `/design src/rmi/transport`), treat that as the target directory. Otherwise ask: "Which directory are you designing? (provide a path relative to the project root)"

Once the target directory is known:

1. **Load reference context** — Glob for all existing `DESIGN.md` files in the project and read each one. These give you architectural context and establish the writing style and level of detail to match.
2. **Load the current document** — If `DESIGN.md` already exists in the target directory, read it. The session will refine it; otherwise you will create it from scratch.
3. **Confirm the scope** — Briefly state what directory you are designing and what you already know about it (from surrounding code or existing docs), then ask the user to describe what they want to design or change.

## Design Session Behavior

- Stay in this mode for the entire conversation. Each exchange with the user is a design discussion — ask clarifying questions, propose structures, reason about trade-offs.
- After every exchange that produces a substantive design decision (a new abstraction, a changed interface, a resolved trade-off, a finalized constraint), immediately write the updated `DESIGN.md` to the target directory using the Write or Edit tool. Do not wait until the end.
- Tell the user what you wrote after each update: one sentence summary of what changed.
- If the user says "done", "looks good", "ship it", or similar, write a final clean version of the document and confirm the session is complete.

## What DESIGN.md Must Contain

Write only what a future AI agent needs to implement the code in this directory correctly. Be concise. Every sentence must earn its place.

Required sections (adapt titles to fit the content):

1. **Purpose / Scope** — What this directory does and what it explicitly does NOT do. List the source files it owns.
2. **Design Goals** — The 3–6 non-negotiable requirements that drive the design choices. Number them.
3. **Key Abstractions** — The central types, interfaces, or modules. For each: name, role, and the invariants it must uphold.
4. **Data Flow / Architecture** — How the pieces connect at runtime. A brief narrative or numbered sequence. Diagrams in ASCII if they help.
5. **Public API Contract** — The surface an external caller sees: function signatures or type names, their preconditions, return values, and failure behavior. Skip internal details.
6. **Design Decisions** — The choices that are not obvious from the code. For each: what was chosen, what was rejected, and the reason. An implementer who doesn't know the history needs this to avoid undoing it.
7. **Constraints and Invariants** — Hard rules the implementation must never break (thread safety, ordering, ownership, error handling style, platform concerns).
8. **Integration Boundaries** — What this directory depends on and what depends on it. Note any shared state or coupling.

Omit any section that genuinely does not apply. Do not add padding. Do not restate what well-named code already says.

## Writing Style

Match the style of the existing `DESIGN.md` files in this project:
- Plain prose, no marketing language.
- Short numbered or bulleted lists for enumerable items.
- Tables for mappings (e.g. error codes, tag values, field layouts).
- No inline code blocks for things that are obvious from context.
- First line of the document is a `# Title` heading.
- Section headings are `## N. Title` (numbered).

## Tools to Use

- `Glob` — find all `DESIGN.md` files and source files in the target directory.
- `Read` — read existing `DESIGN.md` files and relevant source files for context.
- `Write` — create a new `DESIGN.md`.
- `Edit` — update an existing `DESIGN.md`.

Do not modify any source code during a design session. This skill is for design only.
