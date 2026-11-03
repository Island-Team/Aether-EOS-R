# Rust EOS JSONL events

[中文](../../../docs_cn/eos/interfaces/events_cn.md) · [EOS interfaces](README.md) · [P06 task](../../island/implementation-todo.md)

IsOS owns the deployment log store, rotation, cursors and manager-session failures. Each managed Rust EOS process owns the events it produces. This contract covers `aether-runtime`, `aether-http-bridge` and `aether-config-server`; `aether-profile` setup and `--capabilities` are bounded probes, not running modules. The Rust producers emit this P06 envelope; IsOS collection and first-use qualification are separate integration work. The opt-in C++ `eos::lib::LogEvent` tree in [legacy library design](../design/lib.md) has different keys and paths. Its attachments and ActionList audit are outside the 0.x Rust event stream.

## Handoff and transport

IsOS allocates a deployment `run_id` only after prelaunch package, graph, reader and generated-config checks pass. It passes the same `--run-id <id>` to each managed EOS child. The value is 1–64 ASCII letters, digits, `_` or `-`; an invalid or duplicate argument fails before admission. A standalone Rust EOS child with no `--run-id` mints one local ID with the same syntax. EOS writes one UTF-8 JSON object per line to stderr, with no arbitrary diagnostic text mixed into that stream after event initialization. Runtime stdout retains the existing ready/control protocol, and capability/setup probe stdout retains its existing JSON result. IsOS parses the child stream, binds `source`, `core` and `run_id` to the spawned process, rejects a conflicting claim, adds its observed child `pid` only to the normalized stored record, preserves valid envelopes and aggregates them; it never infers readiness from a log line. Runtime emits `source:eos` for its own and backend lifecycle, the bridge emits `source:bridge`, and the config server emits `source:config`. The raw EOS record omits `pid`; a claimed raw `pid` is rejected. On managed launch, IsOS passes `--log-dir` as its private `data/logs/<deployment>/owner/eos/` directory for MuJoCo raw `mujoco.log` capture. That file is separate from the shared JSONL envelope; raw MuJoCo diagnostics are not presented as EOS events. EOS does not write top-level `eos.log` or `events.log`. IsOS alone writes per-source `eos.log`, `bridge.log`, `config.log` and combined `events.log`.

IsOS rechecks the pinned schema-2 MuJoCo receipt before spawn. For a managed launch with `--backend-node`, it also passes `--backend-release-sha256 <digest>` and `--backend-payload-sha256 <digest>` from that exact verified receipt. These new arguments form an optional pair for standalone/manual EOS launch, preserving the existing launch path; when present, both must be lowercase 64-hex and are rejected without a backend launch. EOS checks the graph pin and launch arguments before emitting `eos_package_admitted` with both digests. Without the pair it emits `eos_package_provenance_unavailable` with the exact pin and no digest claim; that warning does not change existing standalone admission. The digests are IsOS-provided provenance, not an independent EOS package signature check. EOS never logs local package paths or raw receipt bytes. A failed IsOS package/Apply preflight belongs to the manager-session `run_id` and optional request `correlation_id`; EOS emits no event because it was not started.

## Envelope

A valid raw EOS event has the following fields. The P06 collector rejects unknown top-level keys; a future extension needs a revised shared contract. No event includes a payload, attachment or raw command arguments. One producer line, including newline, is at most 8064 bytes, reserving 128 bytes for IsOS to add its observed `pid` while keeping the normalized stored line within 8192 bytes. Serialization escapes control characters and emits no embedded newline.

| Field | JSON type / unit | Purpose, absence and validation |
| --- | --- | --- |
| `timestamp` | string / UTC time | Required RFC3339 with `Z` and millisecond precision. Wall time is display metadata; monotonic time drives suppression and deadlines. |
| `level` | string / — | Required `trace`, `debug`, `info`, `warn` or `error`; default emission threshold is `info`, while actionable warnings and terminal failures are never hidden. |
| `source` | string / — | Required `eos`, `bridge` or `config`, matching the IsOS source file. Raw MuJoCo child output is captured separately in a private file and is outside this envelope. |
| `core` | string / — | Required literal `eos`. |
| `node_path` | string or null / — | Saved-graph JSON Pointer `/cores/eos/layers/<layer>/groups/<group>/nodes/<node>`, at most 512 UTF-8 bytes, or null for process-wide events. EOS expands its validated internal four-segment `eos/<layer>/<group>/<node>` path and escapes `~` as `~0` and `/` as `~1` in each segment; it never emits the internal short path or a filesystem path. Current `valid_segment` admits only ASCII letters, digits, `_` and `-`; a node ID containing `~` or `/` fails graph admission, so escaping only constrains the defensive encoder. |
| `event_code` | string / — | Stable lowercase snake-case ID, 1–64 ASCII bytes; the lifecycle codes below are reserved. |
| `run_id` | string / — | Required deployment ID supplied by IsOS, or local ID for standalone launch; 1–64 ASCII letters, digits, `_` or `-`. |
| `correlation_id` | string or null / — | Optional operation/request ID, same syntax and bound as `run_id`; null for startup and unsolicited health changes. |
| `message` | string / — | Required human-readable text, at most 8192 UTF-8 bytes under the shared envelope; EOS uses fixed templates of at most 256 bytes so the complete line fits its 8064-byte producer bound. Never interpolates an error chain, path, payload, environment value or request body. |
| `fields` | object / — | Required allowlisted operational scalars, at most 12 keys and 4096 serialized UTF-8 bytes; shared key syntax is 1–48 ASCII letters, digits, `_` or `-`, while EOS currently emits only the exact keys below. String values are at most 256 UTF-8 bytes with no controls; numbers are finite; Boolean and null are allowed by the shared envelope. No arrays or nested objects. |

`fields` is selected by event-specific constructors, not copied from arbitrary `serde_json::Value`, `Debug`, stderr or tracing span data. The current keys have these additional rules; absent keys are omitted rather than serialized as null. An untrusted diagnostic maps to a fixed reason code and fixed message.

| `fields` key | JSON type / unit | Validation and purpose |
| --- | --- | --- |
| `state` | string / — | One of `starting`, `ready`, `failed`, `stopping`, `stopped`; current process or node transition. |
| `stage` | string / — | Process failure boundary: one of `options`, `profile`, `registry`, `adapter`, `journal`, `backend`, `listener`, `stop`, `sink`. For `eos_skill_execution` only: `queued`, `running`, `completed`, or `failed`. |
| `reason_code` | string / — | Process codes: `invalid_run_id`, `profile_invalid`, `registry_invalid`, `adapter_unavailable`, `journal_unavailable`, `backend_start_failed`, `child_exited`, `listener_bind_failed`, `stop_unconfirmed`, `event_sink_failed`, `other_failure`. `eos_skill_stop_unconfirmed` uses `skill_stop_unconfirmed`; `eos_skill_execution` uses a fixed host-owned `worker.*` code of at most 128 lowercase ASCII letters, digits, `.`, `_`, or `-`; guest text and internal `HostError` codes such as `worker_protocol_io` or `stage_unavailable` are mapped to fixed public codes. Never raw guest text. |
| `package_id`, `package_version` | strings / — | Exact admitted graph pin, at most 96 and 64 ASCII bytes respectively; never a local path. |
| `release_sha256`, `payload_sha256` | strings / SHA-256 | IsOS-verified handoff digests, exactly 64 lowercase hex characters; only appear together. |
| `node_count` | integer / nodes | Admitted node count, `0..4294967295`. |
| `execution_id` | string / — | For `eos_skill_execution` only: host-issued `ex-` identity, at most 80 lowercase ASCII letters, digits, `.`, `_`, or `-`. |
| `worker_reaped` | Boolean or null / — | For `eos_skill_execution` only: whether an actual worker was reaped; null for queued/running transitions with no reap observation. |
| `worker_exit_code` | integer or null / process exit code | For `eos_skill_execution` only: observed worker exit status when available; never a guest-authored status. |
| `suppressed_count` | integer / events | Repeated events omitted since the prior emitted summary, saturated at `4294967295`. |

No secret, credential, token, environment map, raw frame, prompt, primitive argument, provider text, socket path, scene path, attachment or media bytes may enter any field or message. The Rust event stream creates no attachments in 0.x.

A verified backend package record is:

```json
{"timestamp":"2026-11-01T08:00:00.100Z","level":"info","source":"eos","core":"eos","node_path":"/cores/eos/layers/process/groups/mujoco/nodes/sim","event_code":"eos_package_admitted","run_id":"run_7","correlation_id":null,"message":"EOS backend package admitted","fields":{"package_id":"island/mujoco-sim","package_version":"0.1.1","release_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","payload_sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}}
```

A process readiness record is:

```json
{"timestamp":"2026-11-01T08:00:00.123Z","level":"info","source":"eos","core":"eos","node_path":null,"event_code":"eos_ready","run_id":"run_7","correlation_id":null,"message":"EOS runtime ready","fields":{"state":"ready","node_count":3}}
```

A terminal failure example is:

```json
{"timestamp":"2026-11-01T08:01:00.000Z","level":"error","source":"eos","core":"eos","node_path":"/cores/eos/layers/process/groups/mujoco/nodes/sim","event_code":"eos_backend_lost","run_id":"run_7","correlation_id":null,"message":"EOS backend child exited","fields":{"state":"failed","reason_code":"child_exited"}}
```

## Q01 browser command correlation — EOS producer implemented

The loopback HTTP bridge emits one sanitized diagnostic `bridge_primitive_result` event for each valid private `primitive_typed` response and each newly observed terminal status from authorized `result` follow-up. IsOS selected-child admission and P07 replay are separate owner work. This event does not acknowledge physical motion; the Dispatcher journal and governed Stop remain authoritative. A result event has fixed `message: "EOS browser primitive result"`, `level: "info"` except `Failed`, `Rejected` and `OutcomeUnknown` use `warn`, and `node_path: null`. Its `fields` contain exactly `status` (`Completed`, `Failed`, `Cancelled`, `Rejected`, `InProgress`, or `OutcomeUnknown`) and `phase` (`submit` or `followup`). A malformed or contradictory private response produces no result event. No argument, binding, target, grant, reason text, frame, or raw response enters the event.

The public browser `command_id` acceptance remains 1–128 UTF-8 bytes. If an ID is 1–64 ASCII letters, digits, `_` or `-` and does not begin `h_`, `correlation_id` is that exact ID. Otherwise, it is `h_` followed by the first 62 lowercase hex characters of `SHA-256("eos-browser-command-v1\0" || UTF8(command_id))`. Reserving `h_` from the exact branch prevents a raw ID from masquerading as a derived token. The IsosUI-generated `ui-<UUID>` takes the exact branch; a client with another valid ID can compute the token before a P07 filter. The bridge never copies arbitrary UTF-8 to this field. Event write failure does not alter a result or block Stop. A bounded, thread-safe per-command status cache emits the submit result and first observed terminal transition but suppresses repeated `InProgress` polls and duplicate terminal reads. Eviction or bridge restart can repeat a diagnostic without changing the journal. P07 retains first ready/failure anchors through rotation; ordinary command events follow bounded retention and visible gap rules.

```text
{"timestamp":"2026-11-02T08:00:00.100Z","level":"info","source":"bridge","core":"eos","node_path":null,"event_code":"bridge_primitive_result","run_id":"r_demo","correlation_id":"ui-018f","message":"EOS browser primitive result","fields":{"status":"Completed","phase":"followup"}}
```

A nonblocking sink write error permanently disables that bridge process's event stream. It cannot retry a diagnostic in the same process; a missing correlated line fails full Q01 verification. A new bridge process has a new sink and may emit subsequent events without inventing the missing old line.

## Lifecycle and suppression

| Module | Startup, ready, terminal and stop codes |
| --- | --- |
| Runtime | `eos_starting`, `eos_ready`, `eos_start_failed`, `eos_stopping`, `eos_stopped` |
| HTTP bridge | `bridge_starting`, `bridge_ready`, `bridge_start_failed`, `bridge_stopping`, `bridge_stopped` |
| Config server | `config_starting`, `config_ready`, `config_start_failed`, `config_stopping`, `config_stopped` |
| Runtime backend | `eos_package_admitted`, `eos_package_provenance_unavailable`, `eos_backend_ready`, `eos_backend_lost`, `eos_stop_unconfirmed` |
| Runtime native skill host | `eos_skill_execution`, `eos_skill_stop_unconfirmed` |

`eos_skill_execution` has `source:eos`, `core:eos`, `node_path:null`, `level:info` for queued/running/completed and `level:error` for failed. Its fixed message is `EOS native skill execution lifecycle`. It carries the deployment `run_id`, a host-derived `skill_` correlation ID from the public request ID hash, and exactly the safe `execution_id`, `package_id`, `package_version`, `stage`, `reason_code`, `worker_reaped`, and `worker_exit_code` fields. `completed` uses `worker.succeeded` only after confirmed worker reap; an unreaped worker always yields `failed` with `worker.reap_failed` and no result, even if the guest returned success. The package pin is selected and verified by the parent, never supplied by the guest; versions with P01 build metadata such as `0.1.0+build.1` remain valid. `eos_skill_stop_unconfirmed` has `source:eos`, `core:eos`, `node_path:null`, `level:error`, fixed message `EOS native skill stop unconfirmed`, and fields `{"state":"failed","reason_code":"skill_stop_unconfirmed"}`. Its correlation ID is the shutdown correlation when supplied, otherwise null. It covers an unreaped worker or Dispatcher cancellation still pending when the host stop deadline expires. Neither event claims a confirmed physical actuator stop; the Dispatcher journal is authoritative. See the [R04 native host ABI](skills/native/host-abi.md) for admission and cancellation behavior. IsOS now admits these two exact codes in its closed child-event policy. The exact merged-tip retained-segment replay is recorded in the [R04 first-use guide](../develop/host-skills.md#exact-merged-tip-r04-checked-retirement-qualification-2026-11-02); it uses harness-induced rotation, not the automatic size threshold.

Each managed process emits exactly one `eos_starting`, `bridge_starting` or `config_starting` after a valid run ID and event stream are ready. Runtime emits `eos_ready` only after its graph, adapters, backend and control socket are admitted. Bridge/config emit their corresponding `*_ready` after their validated local listener is bound; these records mean local transport readiness and do not claim upstream EOS or whole-deployment readiness. Any startup failure emits one `*_start_failed` terminal record. Runtime backend transitions emit `eos_package_admitted` or `eos_package_provenance_unavailable`, then `eos_backend_ready`, `eos_backend_lost` on an observed direct-child exit, and `eos_stop_unconfirmed` when applicable; R05 later covers live-child socket failure detection. Graceful shutdown emits `*_stopping` then one `*_stopped`; `*_stopped` means process shutdown completed, never that a physical stop was confirmed when `eos_stop_unconfirmed` occurred. An abrupt process kill may have no terminal line, which IsOS reports from its supervisor state. A `*_ready` event is never fabricated for a failed startup. The dispatcher journal remains authoritative for command outcomes and unknown release; a log event is diagnostic evidence, not a command acknowledgement.

A state transition, first failure, readiness, terminal event and safety-stop failure are never rate-suppressed. Identical repeated nonterminal `warn`/`error` keyed by `(source,event_code,node_path,reason_code)` emit immediately once, then at least once per five monotonic seconds while continuing, with bounded `suppressed_count`; `debug` repeats may be summarized on the same window. The suppression table holds at most 256 keys and rolls excess into one `eos_events_suppressed` counter record. No frame, poll, sample or control-loop tick produces an event. A failed stderr write cannot be treated as readiness or confirmed physical stop; the process state and dispatcher result remain authoritative.

The [P06 fixtures](../../../tests/fixtures/jsonl_events/README.md) freeze the exact envelope, provenance, redaction, lifecycle and suppression boundaries. [Runtime event design](../design/runtime/events.md) maps these to current Rust entrypoints. IsOS P07 owns retention, cursor recovery and source aggregation.
