# `egia.lib`

`egia.lib` contains leaf configuration, layout, logging, and utility code. It has no dependency on core runtime packages.

## Configuration ownership

- `egia.toml` selects model, launch switches, perception, decision, memory, monitor, logging, and the northbound listener;
- `models.toml` defines decision-model bindings, including the 262144-token default context window and per-call generation limit;
- `modes.toml` selects a profile under `profiles/`;
- profiles define EOS backends and `[primitives].expose_operator`;
- `prompts.toml` is the only model-prompt source;
- `missions.toml` defines the optional strict whitelist of named canonical Missions and protected initial plans; `[decision] load_mission_templates` controls runtime loading and defaults to `false`;
- `safety/gate.toml` defines deterministic egress bounds.
- product `island.toml` selects prefix-relative shared-resource roots and component launch paths.

CLI arguments and documented environment variables may override selections and runtime roots. Unknown fields and invalid clean-break shapes fail validation; there is no migration for removed profile sections.

`--island-config` selects a product config for EGIA's shared-resource lookup. Relative paths are resolved below the Island prefix. The product launcher always passes its resolved `--config` path through to EGIA, so a custom composition cannot silently fall back to the default `island.toml`.

## Prompt configuration

The prompt loader reads `etc/egia/prompts.toml` directly. Each lane, including Mission-history summarization, owns unique named static sections, and each context entry declares `dynamic = true` plus a retention priority. The loader rejects duplicates and malformed classifications. There is no factory-root or shared prompt fallback. The runtime orders stable dynamic content before frequently changing content, persists lifetime change counters below the state root, ignores deployment-time prompt edits when establishing a new process baseline, and applies a smaller guaranteed-fit inner budget before model transport.

## Mission template configuration

The optional `etc/egia/missions.toml` catalog is independent of global lane instructions and is not loaded at runtime unless `[decision] load_mission_templates = true`. Each template has a unique ID and positive version, a canonical Mission, and a non-empty dependency-checked list of semantic steps. Step IDs and dependencies are strict, dependency graphs must be acyclic, and unknown fields fail validation. A missing file means that no templates are available. Config models remain leaf data; core admission converts selected template steps into runtime Plan records.

## Layout and logging

Public resources resolve through checkout or installed layout roles. Every `share` entry may be an arbitrary prefix-relative directory. The resolver indexes all files as one logical tree and fails on a missing root or duplicate relative path. At EGIA startup, every `.json` file below logical `contracts/` and `primitives/` is loaded and required to be an object with a unique non-empty `$id`; payload validation builds the cross-schema reference registry from that complete set. Runtime writes are restricted to state, cache, log, run, and temporary roots. Structured logging assigns every process a `run_id` and can bind `correlation_id`, `mission_id`, `step_id`, `backend`, `command_id`, and `execution_id` across decision, safety, I/O, and EOS egress.

## Data flow and conditions

Configuration is built in order from packaged model defaults, deployment files, selected mode and profile, model bindings, then documented environment and CLI selections. Parsing is strict: missing required files, unreadable or invalid TOML, unknown fields, removed legacy shapes, duplicate backend IDs or addresses, unresolved model bindings, inconsistent controller and reasoner selections, malformed prompts, invalid mission dependencies, non-loopback sandbox exposure, missing safety policy, and invalid absolute roots fail at composition with the source path in the error. Zero configured backends is permitted only where the selected launch behavior can remain intentionally disconnected; it never creates an implicit endpoint.

The dotenv reader treats a missing file as an empty overlay, ignores comments, blank lines, and malformed assignments, preserves existing process variables, and reports only names it loaded. Quoted values are unwrapped according to the supported escape rules; it does not evaluate shell syntax. Secrets remain in environment lookup and are never copied into structured config output.

Logging sends one sanitized event through configured console, human-file, and replay sinks with sink-specific levels and renderers. Logging configuration and each sink reject unknown keys. Recursive sanitization redacts secret-bearing keys and URLs, excludes primitive arguments, arbitrary results, prompts, transcripts, provider payloads, images, and audio, renders byte values as type-and-size metadata, and bounds nested collections, depth, and strings before any sink sees them. Correlation and lifecycle context is scoped to a request or execution and cleared afterward.

Under Island supervision, the already-sanitized structured event is also offered to the session collector through a bounded nonblocking Unix datagram. Island supplies `ISLAND_SESSION_ID`, `ISLAND_COMPONENT`, and `ISLAND_SESSION_LOG_SOCKET`; loss of that runtime sink never blocks EGIA or replaces its component files. The collector overrides reserved envelope fields and sanitizes metadata again before persistence.

The append-only replay lives at `var/lib/egia/egia.jsonl`, rotates at 32 MiB, and retains five backups; the terminal console follows inode replacement as well as file shrinkage. Human-readable diagnostic files remain below `var/log/egia`; the replay-owning launcher removes stale `.log` routes before child processes start, while a child may clean only an explicitly assigned file subtree. Repetition suppression uses a 4,096-entry least-recently-used index keyed by the complete sanitized event identity, including semantic IDs. It emits counted summaries after the window and at handler shutdown, while audit and terminal outcome events always pass through. Temporary lifecycle verbosity restores the configured console level. Logging setup failure is surfaced rather than silently discarding all diagnostics.

The model client accepts text and supported image inputs, converts images to explicit MIME data URLs, and emits the provider's chat-completions request shape. Non-stream responses require a usable choice; streaming consumes valid SSE deltas and always closes the response. HTTP failure includes a bounded response body, while URL, timeout, decode, malformed JSON, empty-choice, and interrupted-stream failures become `ModelClientError`. Missing usage fields use explicit unknown/default values and never alter the returned text.

The documentation corpus path may be empty (retrieval disabled), absolute, or relative to the resolved `docs/egia` root. The packaged value `.` indexes the complete current EGIA documentation tree. A missing corpus degrades to an empty documentation index with a warning; unreadable or unsupported files are not executed or interpreted as configuration.
