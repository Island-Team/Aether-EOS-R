# Coding and Documentation Conventions

**Convention version: 1.3.1**

This shared convention applies to source code, interfaces, and hand-written documentation. A component guide may add stricter rules for its own scope. Public wire identities and compatibility rules remain governed by their owning contracts. The convention version is independent of product, package, and wire versions. Change English and Chinese copies together. Compatible additions increment the minor version; corrections increment the patch version; removal or replacement of an established rule increments the major version.

## Source and module ownership

Give each module one concern and one clear owner. Separate admission and validation, domain decisions, runtime state, transport, and presentation so a field has one authoritative meaning. Depend on public contracts across process or component boundaries, not another component's private implementation. Keep functions focused. Put nontrivial algorithms and failure handling behind testable interfaces; thin wrappers need no artificial test module.

Use names from the domain. Prefer one-word filenames; use short established abbreviations if needed. Preserve stable public command and wire names even when internal names change. For a public module set or substantial subsystem, provide an overview explaining ownership, dataflow, and links to focused pages. A focused page links back to its overview. Explain data origin, admission, transformation, lifecycle, observable failure, and verification before listing source files.

## Inline documentation

Comments and docstrings are in English and explain non-obvious constraints, units, ownership, concurrency, safety, or surprising failures beside the relevant code. Do not repeat a clear name or operation. Public C/C++ interfaces describe ownership, blocking, and error behavior when callers need it. Update comments when behavior changes; keep detailed design in documentation.

## Documentation structure and writing

Keep hand-written documentation under the owning product or component in `docs/`; mirror its reading path in `docs_cn/`, with `_cn` before a translated filename. Maintain equivalent headings, field meanings, examples, and relative links in both languages. Each tree has a short entry-point README. Organize pages by reader task: `usage/` for setup and operation, `debug/` for running problems, `develop/` for extension work, `interfaces/` for contracts, `design/` for architecture, `layers/` for type catalogs when applicable, `legacy/` for historical compatibility, and `maintenance/` for releases and work tracking. Do not edit generated references by hand.

Write like an article: introduce a term before its first use, give each paragraph one idea, and make a standalone page understandable without its parent. Keep prose paragraphs on one physical Markdown line. Use bullets for sequences or checklists, tables for useful comparisons, and short fenced examples when exact syntax matters. Avoid duplicating payload definitions in design, layer, or usage pages. Mark future behavior **Target design — not implemented** and link its executable TODO. Document implemented behavior in present tense.

Order first-use guides as an operator encounters the system: prerequisites, package discovery and verification, configuration and validation, Apply or equivalent admission, launch and readiness, live monitoring, logs and debugging, stop, upgrade, and recovery. Start a log guide with finding the current run and its first ready or failure event.

## Interface contracts

Every hand-written HTTP, WebSocket, RPC, IPC, plugin, skill, hardware, or data contract belongs under the owning component's `interfaces/` directory and its Chinese mirror. Identify caller and owner, transport or invocation, authentication and admission, input and output, lifecycle and timeouts, errors, and compatibility. For every public field state type, unit (or “—”), purpose, absence or default behavior, validation, and compatibility rule. Use small valid request, success, and error examples with actual wire names. Keep examples synchronized with schemas, handlers, and tests. Design pages explain why and link to the single owning contract.

For REST APIs, use one domain page per first path segment after the API root. Each page covers **Overview**, **Authentication**, **Interfaces**, and **Routes** in a readable order. Use a `## METHOD /path` heading per endpoint and show who may call it, what to send, success, and exact validation, authorization, not-found, and server-fault codes where applicable. Document JSON envelopes and exceptions, media types, enum literals, bounds, ID formats, and timestamp formats. Put shared authentication details in one auth page when repetition would be substantial. Keep endpoint bodies in their domain pages. A client mapping belongs in its own design page, not as alternate wire names in JSON examples.

Preserve existing field numbers and names for compatible changes; add optional fields with documented defaults and make new readers accept old data. Breaking changes require a new contract identity and migration guide. A product version below 1.0.0 does not imply that unrelated wire contracts share its stability.

## Code style and tooling

Repository configuration is the source of truth for formatting, lint, indentation, and type checking. Configure editors to use it; do not require an editor-specific settings folder. Use two spaces and no tabs unless an authoritative language formatter requires otherwise. Format, lint, and test with the tools declared by the repository before committing. Keep mechanical formatting commits separate from behavior changes when the diff would hide the behavior. Do not commit logs, build output, caches, credentials, or machine-specific files.

For Rust, follow the official Rust Style Guide and the repository-root `rustfmt.toml` across all targets. Do not add crate-local formatter overrides. Run `cargo fmt --all` and verify with `cargo fmt --all -- --check`; run Clippy and relevant tests. Keep unit tests near the implementation and integration tests in the package's `tests/` tree. Use structured logging, avoid secrets, propagate errors with context, and avoid blocking work in async contexts.

For TypeScript, use strict types, the repository linter and formatter, `.tsx` for JSX, and `.ts` for logic. Prefer explicit exported return types and accessible semantic controls. Centralize network access. Preserve shared visual tokens, responsive layouts, keyboard access, and at least 44×44 px touch targets. For Python, target the repository-pinned version (otherwise 3.12), type public functions, and keep type-check mode in shared configuration. For C/C++, use repository-pinned standards and root formatter configuration; keep public headers separate from private helpers and define each compiled module's dependencies explicitly in CMake.

## Language and interface implementation details

Rust applications may use `anyhow` for application errors and libraries may use `thiserror` for typed errors; prefer `Result` over panics and add context when propagating. Use `tokio` for async Rust where the repository uses that runtime, and move blocking work to a blocking worker. Name tests like sentences, such as `test_<what>_<condition>_<expected>()`. Test the boundary with mocks or stubs rather than depending on live hardware in unit tests.

TypeScript projects use the configured ESLint and Prettier rules. Prefer interfaces for object contracts and literal unions for finite choices when those represent the wire clearly. React projects use functional components. Components and entry files use `PascalCase.tsx`; helpers use concise lowercase names. Co-locate styles when practical. Follow the repository's semicolon and quote policy. Centralize HTTP and WebSocket code in a service layer. A frontend build is self-contained unless its documented deployment explicitly permits runtime external assets, and client-side routing needs a host fallback to `index.html`.

Frontend controls use semantic landmarks; actions use buttons and navigation uses anchors. Every control has visible text or an accessible name. Use the shared theme tokens for colors, typography, spacing, status, and focus; maintain readable contrast. Desktop controls follow the product's shared compact height, ordinarily 28–30 px; touch targets are at least 44×44 px. Work pages favor scan-friendly rows, visible state, bounded scrolling, and keyboard access. Explanatory pages may use more space. Critical stop and failure state stays visible in floating, docked, and narrow layouts. Use responsive grid for page shells and flexible layout within components.

C and C++ modules target C++17 or later unless the repository pins a newer version. Set the CMake language standard explicitly and avoid undocumented compiler extensions. Follow the configured Google-style formatting with a 100-column target where applicable. Each compiled C/C++ module has its own `CMakeLists.txt`, declares its source files and public/private include paths and link dependencies, and puts generated interface targets behind a helper rather than referencing generated paths directly. Root CMake adds modules in dependency order. Public ABI and class headers belong in `include/`; private helpers stay within the module or `detail/` and are never included outside it. Brief Doxygen is appropriate for caller-visible ownership, units, thread safety, blocking, and errors; generated Doxygen is never hand-edited.

Python follows its configured Google-style guidance, with two-space indentation where the repository adopts this convention. Use a brief docstring for a non-obvious contract and type hints on public functions. Put at least standard type-checking configuration in `pyproject.toml` when Python is part of the project. Explain non-obvious numerical, concurrency, hardware, and failure assumptions in English comments, not obvious assignments.

When adding configuration, update its struct or schema, checked-in sample, defaults, valid range, and owning interface or usage page in the same change. Keep secrets out of samples and logs.

## Module verification and delivery

A module's focused documentation names its stable inputs and outputs, types, ownership, units, timing, caller responsibilities, compatibility, state transitions, ordering, degraded dependencies, known limits, and deliberately unsupported cases where relevant. Describe the algorithm choice and invariant rather than copying source signatures. Link to generated API details when useful. Keep simulations, replays, mocks, hardware, and live services behind the same public interface where practical. The nearest overview defines the shared boundary and end-to-end dataflow; focused pages carry condition-level detail.

A new or redesigned module is delivered in this order: define the problem, success criteria, constraints, and non-goals; research dependencies and failure modes; define the public interface; update the overview, focused design, and contract; build the smallest useful implementation; add unit and integration evidence where warranted; finish formatting, lint, tests, and both documentation languages; commit the coherent change. For a small change, update affected contracts and design, implement and test the changed behavior, and update only the relevant documentation. Record reproducible commands and pass/fail results rather than pasting long logs.

Unit tests may live in a local `tests/` directory or an inline test module according to the language. A test file should target one unit, feed mock or stub input, and assert expected output. Cross-module smoke tests live near the composed process. Name the method used—smoke, unit with mock, stubbed integration, simulation, replay, benchmark, hardware-in-loop, or manual acceptance. Add tests for large modules, non-obvious algorithms, and expensive-to-debug behavior. Verify failure and stop behavior as well as success.

## Event and release records

For machine-readable events, use sanitized UTF-8 JSON Lines. The shared envelope has an RFC3339 UTC `timestamp`, `level` (`trace`, `debug`, `info`, `warn`, or `error`), `source`, owning `core` or service, optional canonical `node_path`, stable `event_code`, `run_id` when a run exists, optional `correlation_id`, a human `message`, and bounded `fields`. Emit an informative startup or ready event, then state changes and actionable failures. The deployment owner aggregates sources and defines per-source files, rotation, and cursor reads; each component remains the structured producer.

Version graph formats, node type IDs, protobuf namespaces, HTTP routes, packages, and socket protocols independently. Release notes use the same short order: version and date, operator-visible changes, compatibility and migration, installation or upgrade, known limits, and verification. A release version change does not automatically change another contract identity. Do not infer stability from a pre-1.0 product version.

## Review and publication

Before sharing a branch, confirm formatting, lint, focused and integration tests, contextual error propagation, useful logging, current interface and configuration documentation, translation parity, and no secrets or generated artifacts in the diff. Keep formatting-only changes separate from behavioral changes when they would obscure review. The repository's publication workflow governs whether a branch is pushed or reviewed through a PR; when a PR is used, describe behavior, verification, and documentation, update the branch against its target, and follow that repository's merge strategy. Do not force-push a shared branch without team agreement.

## Testing and observability

Add unit tests with mocks for substantial module logic and stubbed integration, simulation, replay, or hardware tests where a boundary requires them. Test success, rejection, dependency failure, stop, and recovery as relevant. Name the method and reproducible command in the module document. Benchmarks use fixed workloads and report environment, throughput, median, and p99 before claiming a performance target.

Emit sanitized structured events with stable codes and bounded fields. Include timestamp, level, source, owning core or service, run or correlation identity when applicable, and an actionable message. Emit startup and meaningful state transitions or failures; avoid repetitive success noise. Do not log secrets, raw media, prompts, primitive arguments, or unbounded provider payloads. The owning interface describes log retention and cursor recovery.

## Change and release workflow

Start changes on a topic branch from the current target; do not develop directly on the integration branch. Keep commits focused with `feat:`, `fix:`, `docs:`, `refactor:`, `test:`, or `chore:` prefixes. Update code, contract, tests, and English/Chinese documentation together when behavior changes. Review the diff and local checks before publishing. Use the repository's review and merge process when publication is requested.

Release notes state version and date, operator-visible changes, compatibility and migration, install or upgrade steps, known limits, and reproducible verification. Label candidates and target designs explicitly. Historical release tags remain immutable.
