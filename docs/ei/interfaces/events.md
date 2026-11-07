# EGIA shared deployment events

[中文](../../../docs_cn/ei/interfaces/events.md) · [IsOS collector contract](../../isos/interfaces/cores/events.md) · [Design](../design/events.md) · [First use](../develop/build-and-run.md) · [P06 task](../../island/implementation-todo.md)

**Implemented P06 envelope with A03 closed-policy revision.** EGIA owns the meanings and safe construction of its `egia` events. IsOS owns run allocation, selected-child pipe binding, aggregation, rotation and cursor reads. The selected managed EGIA process writes one strict UTF-8 JSON object plus `\n` per event to stderr; stdout and the existing private replay are separate. Only the selected direct-child stderr pipe can establish `source:egia` in IsOS. A listener, PID or log line does not itself establish deployment health.

## Envelope and identity

The raw child line is at most **8,064 bytes including newline**. EGIA sanitizes and shortens its fixed message at a UTF-8 boundary until the complete line fits. IsOS adds the observed PID and stores at most 8,192 bytes. The exact top-level fields are below; no producer-supplied `pid` or extension key is allowed.

| Field | JSON type / unit | Purpose, absence, validation and compatibility |
| --- | --- | --- |
| `timestamp` | RFC 3339 UTC string / milliseconds | Required actual event time; preserve milliseconds and UTC `Z`. |
| `level` | `trace`, `debug`, `info`, `warn`, `error` / — | Required severity. The policy below pins the level for each EGIA code. |
| `source` | string / role ID | Required literal `egia`; IsOS binds it to the selected direct child. |
| `core` | string / owner ID | Required literal `egia`. |
| `node_path` | canonical JSON Pointer or null / saved graph | Optional, normalized to null. A nonnull value ends at an admitted EGIA node, not a member path; maximum 512 UTF-8 bytes. Process lifecycle and discovery codes require null. |
| `event_code` | lowercase ASCII string / code | Required, 1–64 bytes. Only the exact codes in [the role policy fixture](../../../ei/tests/fixtures/jsonl_events/egia-role-policy.json) are emitted. New codes require reviewed owner and IsOS policy changes. |
| `run_id` | ASCII string / run ID | Required, 1–64 `[A-Za-z0-9_-]`. Managed `ISOS_RUN_ID` must match `r[A-Za-z0-9_-]{1,63}` before startup; a missing or malformed managed binding fails before an EGIA event is emitted. Standalone EGIA mints `l` plus 32 lowercase UUID hex digits. A standalone process rejects an unexpected `ISOS_RUN_ID` rather than assuming manager authority. |
| `correlation_id` | ASCII string or null / request ID | Optional, normalized to null; same syntax and length as `run_id`. Carries the admitted request or operation correlation, never user text. |
| `message` | string / UTF-8 bytes | Required fixed text from the role policy; never an exception string, prompt, command or provider response. Further shortened only for the line bound. |
| `fields` | object / bounded metadata | Optional, normalized to `{}`. At most 12 code-allowed scalar keys and 4 KiB serialized; keys are 1–48 ASCII letters, digits, `_` or `-`; strings at most 256 UTF-8 bytes without controls. The fixture pins required/optional fields and enum domains. |

For example, after EOS discovery and safety initialization:

```json
{"timestamp":"2026-11-01T10:00:02.000Z","level":"info","source":"egia","core":"egia","node_path":null,"event_code":"egia_ready","run_id":"r7-cde","correlation_id":null,"message":"EGIA ready","fields":{"generation":3}}
```

The IsOS supervisor rejects a mismatched role/run, malformed or overfull line, unknown code, invalid node, unsafe field, duplicate JSON key or raw `pid`. EGIA records a safe private diagnostic when its managed binding is invalid; no shared event may claim a fabricated deployment run. IsOS emits its own child-exit/start failure for a pre-event crash.

## Closed role policy

The [machine role policy](../../../ei/tests/fixtures/jsonl_events/egia-role-policy.json) is the normative exact list, message, severity, node rule, typed fields and enum values. Optional field names end in `?` in that fixture; the wire key omits `?`. The 24 codes are:

| Kind | Codes | Emission and suppression rule |
| --- | --- | --- |
| Lifecycle | `egia_starting`, `egia_ready`, `egia_start_failed`, `egia_stopping`, `egia_stopped` | One starting after validated config/run binding and event sink; one ready only after complete EOS discovery, model binding and safety gate; one terminal start failure or stopped outcome. `egia_stopped` uses `info` for `state:stopped` and `error` for `state:stop_unconfirmed`; the opposite pairings are invalid. Never suppress. |
| Discovery | `egia_discovery_lost`, `egia_discovery_restored` | Emit on actual complete-generation loss or restoration, not each poll. Never suppress the first transition. |
| Mission | `egia_mission_started`, `egia_mission_terminal`, `egia_mission_restored` | Emit after durable state transition or checkpoint restoration. A terminal outcome is never suppressed; `unknown` does not mean physical success. |
| Stage and gate | `egia_stage_transition`, `egia_proposal_created`, `egia_admission_decided` | Emit typed stage acceptance and the pre-gate proposal by one validated operation correlation ID; admission follows durable reservation before EOS egress. |
| Safety | `egia_safety_rejected`, `egia_safety_latched`, `egia_safety_reset_requested`, `egia_safety_reset_completed`, `egia_safety_reset_refused` | Emit the first refusal, latch, and each one-shot reset result immediately. A distinct correlation ID keeps its first refusal. |
| Monitor | `egia_monitor_episode` | Emit fault and recovery transitions, not each heartbeat. |
| EOS operation | `egia_operation_submitted`, `egia_operation_terminal`, `egia_operation_unknown` | EGIA sends the validated operation ID as EOS's durable idempotency key. Submit means confirmed admission only; terminal requires the EOS receipt. If Submit loses its response, EGIA cannot know whether EOS admitted movement: emit unknown and recovery required, mark stop unconfirmed, and never replay movement. Never suppress terminal or unknown. |
| Recovery | `egia_recovery_required` | Emit when checkpoint/rediscovery or unknown action needs reconciliation. Never suppress. |
| Bounded repetition | `egia_events_suppressed` | Report a saturated positive `suppressed_count` for repeated noncritical diagnostics; never claim a dropped safety or terminal event. |

`generation` is a positive EOS discovery generation, not a readiness substitute. `mission_id` and `operation_id` are opaque IDs matching `[A-Za-z0-9_-]{1,64}`. EGIA validates caller-supplied command IDs against that grammar before any EOS discovery or submission; an invalid ID fails with `invalid operation ID` and no actuator call. Reason and outcome values are closed enums in the fixture; unknown internal failures map to a reviewed generic code, never an arbitrary exception. A03 adds the fixture schema `egia-jsonl-role-policy-a03-v2`; IsOS must mirror this exact policy before accepting the new codes from a selected child. An admitted EGIA `node_path` remains optional and validated against the applied graph; a graph wire never becomes an actuator route.

## Isolation, redaction and loss

The existing `var/lib/egia/egia.jsonl` replay remains EGIA's rotating owner record (32 MiB, five backups); human diagnostics remain under `var/log/egia`. They use the existing sanitization pipeline but are not copied verbatim to IsOS. Managed stderr writes only the closed shared envelope. EGIA duplicates the selected stderr pipe for the event sink, then redirects process file descriptor 2 to `/dev/null`: native writes, warnings and tracebacks cannot contaminate the shared pipe or persist unsanitized bytes. Raw prompts, transcript, media, model/provider bodies, primitive arguments/results, URLs, filesystem paths, credentials, env names/values and exception chains never enter `message` or `fields`. IDs are validated before use; no arbitrary text is shortened into an allowed field.

The first readiness, state transition, warning, failure and terminal record is emitted immediately. Repeated identical noncritical diagnostics are keyed by `(run_id,event_code,node_path,reason_code,correlation_id)`, use a monotonic 60-second window and a 256-entry bounded table, and produce a counted `egia_events_suppressed` record at window/terminal flush; counts saturate at `4294967295`. The full private replay may retain more sanitized detail. Shared stderr write failure is tracked in a bounded private counter; it cannot block safety stop or EOS result handling. EGIA never invents a readiness or terminal acknowledgement after collector loss. IsOS status and EOS's Dispatcher receipt remain authoritative.


The producer is `src/egia/lib/shared_events.py`, with the reviewed code and field table in `shared_event_policy.py`. `egia --managed` validates `ISOS_RUN_ID` before logging setup; a rejected binding writes only a fixed private diagnostic and exits. The selected managed logger keeps its private replay and file sinks while reserving stderr for this exact JSONL projection. Owner call sites emit startup, complete-discovery readiness, shutdown, mission checkpoint, safety, EOS operation and recovery transitions. An abrupt failure after ready remains a supervisor-observed exit rather than a fabricated stop confirmation.
