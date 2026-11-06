# `egia.io`

`egia.io` is the northbound adapter. It projects runtime state to WebSocket clients and converts operator input into typed domain requests. Domain ownership stays in core modules.

## Published state

Injected stores hold replaceable snapshots for health, backends, missions, context, the planner prompt, model I/O, primitives, native skills, scene, and body parts. Streams send the latest snapshot and then newer versions. Source aliases and device IDs remain attached to multi-robot state.

## Operator input

- chat/hearing input becomes an admitted hearing observation;
- `/missions/command` admits server-attributed operator missions and performs revision-checked updates, priority changes, and cancellation of pending work;
- mission review enters the memory review path;
- `/primitive/invoke` creates an explicitly targeted primitive request and sends it through registry validation and the ordinary safety gate.
- `/skills/command` resolves a configured robot and proxies strict exact-identity skill operations to its dedicated EOS `SkillService` client;
- `/skills/execution` validates a single cursor request and relays complete ordered execution snapshots until the EOS watch ends.

Clients cannot supply mission source, origin, score, lifecycle state, or plans. Mission commands change planning input, not the execution boundary: no operator route bypasses primitive validation, policy, or safety. There is no Agent-local execution endpoint and no raster-map route.

## Readiness

Health and backend state remain visible while mandatory EOS discovery retries. Other routes are readiness-gated and return an unavailable frame when no producer has published data. Mission commands require both runtime readiness and an injected mission table. Primitive and native-skill catalogs are independent: optional skill discovery may be unavailable while backend execution is ready. Skill command routes accept ready or degraded EGIA state when an injected robot client exists, because SkillService availability is reported independently by EOS.

See the [northbound wire reference](northbound.md) for routes and payloads.

## Data lifecycle and conditions

Each replaceable store owns one immutable snapshot and a monotonically increasing in-process version. A subscriber receives the current snapshot first, then waits for a strictly newer version; replacement wakes all waiters. An empty store yields no domain value, polling observes shutdown at bounded intervals, and stop returns `None` rather than fabricating a final frame. Part state is keyed by stable robot and device identity, so replacement or removal cannot merge similarly named devices.

Catalog projection transforms the internal registry into operator-filtered primitive descriptors and a separately versioned native-skill availability view. Entries are deterministically sorted. Missing backends, optional skill unavailability, and an empty catalog are valid snapshots; malformed discovery data never reaches this layer because link admission rejects it atomically. Scene projection preserves attribution and distinguishes an empty scene from no scene received.

The server treats listener bind failure as startup failure. `/health` remains available during startup; other routes report unavailable until their producers exist. Connections with no `Origin` and the configured Web UI origins are accepted; disallowed origins and unknown paths close without invoking domain logic. Invalid JSON, invalid typed requests, queue saturation, missing plans or mission documents, revision conflicts, gate rejection, egress failure, and request timeout have distinct wire errors described in [northbound.md](northbound.md).

Streams unsubscribe and cancel their wait tasks when a client closes. Periodic health and backend streams continue without state-version triggers; versioned streams emit only replacements. Binary input must decode as UTF-8 before JSON validation. A valid backend acknowledgement with `accepted=false` remains an invocation result, while transport or malformed acknowledgement is `egress_failed`; a full observation queue may drop the follow-up result observation without changing the acknowledgement already returned to the caller.
