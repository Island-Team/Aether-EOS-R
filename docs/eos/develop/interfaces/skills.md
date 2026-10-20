# SkillService process interface

[中文](../../../../docs_cn/eos/develop/interfaces/skills_cn.md)

[Interface overview](README.md) · [Workflows](workflow.md) · [Write a skill](skills.md) · [How skills work](../../design/runtime/skills.md) · [Primitive calls](actions.md)

`island.eos.v1.SkillService`, declared in `share/contracts/eos/skills.proto`, is the stable native gRPC interface for skill discovery and execution. It is independent of the native authoring ABI and has no synchronous run-to-completion RPC.

| Operation | Purpose | Streaming |
| --- | --- | --- |
| `ListSkills` | List admitted descriptors | No |
| `GetSkill` | Resolve one exact name/version | No |
| `StartExecution` | Create an idempotent logical execution | No |
| `GetExecution` | Read the latest complete snapshot | No |
| `WatchExecution` | Replay/follow lifecycle snapshots | Server stream |
| `TerminateExecution` | Request cooperative termination of one execution | No |
| `TerminateExecutions` | Idempotently terminate active executions for one exact skill | No |
| `ListExecutions` | List current snapshots for retained executions | No |
| `SignalExecution` | Queue one typed application SIGNAL | No |
| `FollowExecutionJournal` | Replay/follow lifecycle, emitted SIGNAL, and journal records | Server stream |
| `MonitorExecutions` | Follow a filtered process-like execution view | Server stream |

These operations use process semantics rather than mutable skill definitions: `StartExecution` is create; get, list, and watch operations are read; `SignalExecution` is update; and terminate operations are cooperative control. There is no delete RPC. Terminal records remain observable until bounded retention reaps them.

## Catalog

`ListSkills(ListSkillsRequest)` returns all deployment-admitted skills, sorted by exact `{name, version}` identity. `GetSkill(GetSkillRequest)` resolves one exact identity or returns `NOT_FOUND`. A `SkillDescriptor` contains its `SkillReference`, description, parameter and result JSON Schemas, exact primitive dependencies, readable resource URI templates, SIGNAL declarations, claim templates, declared inference authorities, and reentrancy flag. Inference authorities are the typed `VAD`, `HUMAN`, `HUMAN_POSE`, and `FACE_RECOGNITION` values admitted for that skill; they do not expose model IDs. The descriptor deliberately does not expose native library metadata, build IDs, provider types, primitive acknowledgements, or a registration operation.

Accepted and emitted SIGNAL declarations are separate. Each contains a name and object-valued payload schema. A claim template contains a kind (`DEVICE` or `RESOURCE`), a mode (`SHARED_READ` or `EXCLUSIVE_USE`), and an RFC 6901 pointer into start arguments. The pointer must be non-empty, begin with `/`, and resolve at start to a non-empty string. A SIGNAL name matches `^[a-z][a-z0-9]*(\.[a-z][a-z0-9_-]*)+$`; the same name may occur once in each direction. `CANCEL` is uppercase and manager-reserved. Startup admits a descriptor only after its schemas and declared capabilities are valid for the deployment. Installation and authoring remain deployment operations; an empty admitted catalog is valid.

## Starting an execution

`StartExecution` requires:

- an exact skill name and version;
- arguments represented by an object-valued `google.protobuf.Struct` and valid against the admitted parameter schema;
- a non-empty, bounded `request_id` used as an idempotency key.

`timeout` is optional. Omitting it creates an execution with no deadline. When present, it must be a positive `google.protobuf.Duration` no greater than one hour and preserves the deadline and timeout behavior of earlier requests.

The request ID is 1–96 bytes and the argument JSON representation is at most 1 MiB. Start idempotency is manager-global by `request_id`, not scoped to a skill identity.

The response is the first complete `SkillExecution` snapshot. Repeating a request with the same retained `request_id` and identical content returns the existing execution. Reusing the ID with different content returns `ALREADY_EXISTS`. Deduplication ends when the process restarts or the bounded record is evicted. EOS derives its private root primitive command identity from `request_id`; command IDs never cross this public interface.

```protobuf
StartExecutionRequest {
  reference { name: "observe_human" version: "1.0.0" }
  arguments {
    fields { key: "robot_name" value { string_value: "robot-0" } }
    fields { key: "gimbal_device_id" value { string_value: "gimbal-0" } }
    fields { key: "speaker_device_id" value { string_value: "speaker-0" } }
  }
  request_id: "egia-plan-8-step-2"
  timeout { seconds: 30 }
}
```

## Snapshots and watching

`GetExecution` returns the current complete snapshot. `WatchExecution` first replays every retained snapshot whose sequence is greater than `after_sequence`, then follows live transitions and closes after the terminal snapshot. Passing zero starts at the oldest retained snapshot; reconnecting with the last processed sequence avoids duplicates. If the execution is already terminal and `after_sequence` is at or beyond its terminal sequence, the stream closes successfully without an event. A client disconnect ends only that stream and never cancels the execution.

Every snapshot contains the execution ID, request ID, exact skill reference, typed status, lifecycle `sequence`, `latest_journal_sequence`, cancellation flag, optional `queue_reason`, resolved claims, current `active_device_claims`, stable reason code, bounded diagnostic message, result, lifecycle timestamps and durations, current activity, and execution accounting. Accounting includes capability/SIGNAL/observability counts, dropped records, and aggregate wait/capability durations. `deadline_at` is present only when the start request supplied `timeout`. `queue_reason = "claim.conflict"` means the execution is still queued because its complete claim set cannot yet be acquired. Status progresses from `EXECUTION_STATUS_QUEUED` to `EXECUTION_STATUS_RUNNING` and one of:

- `EXECUTION_STATUS_SUCCEEDED`;
- `EXECUTION_STATUS_REJECTED`;
- `EXECUTION_STATUS_CANCELLED`;
- `EXECUTION_STATUS_TIMED_OUT`; or
- `EXECUTION_STATUS_FAILED`.

`EXECUTION_STATUS_UNSPECIFIED` is never an admitted runtime state. The result is meaningful only when supplied by the terminal outcome; clients must use the status and reason code rather than parsing the diagnostic message. The message is optional, sanitized operator text capped at 1,024 bytes and is carried unchanged through Get/List/Watch and north HTTP/SSE.

Each resolved claim exposes its kind, mode, and concrete identity. A non-reentrant provider adds a visible `PROVIDER_CONTEXT`/`EXCLUSIVE_USE` claim. Claims conflict only when kind and identity match and either side is exclusive. `ListExecutions` returns one current snapshot for each retained record; clients must not depend on response order.

`TerminateExecution` idempotently requests cooperative termination and returns the current snapshot. A queued job can terminate without entering running; an in-process native callback cannot be preempted, but later host calls are denied and termination or deadline determines the canonical terminal state after it returns. EOS exposes neither numeric POSIX signals nor hard kill.

## SIGNALs and execution journal

The service adds three operations without changing the existing ones:

- `ListExecutions` returns the current snapshot of every retained execution, providing the process-manager-like `ps` view.
- `SignalExecution` accepts a target execution ID, idempotency request ID, declared SIGNAL name, and object-valued payload, and returns an admission receipt. A receipt confirms validation and queueing, not consumption.
- `FollowExecutionJournal` replays and follows a per-execution stream whose sequenced `oneof` is a lifecycle snapshot, emitted SIGNAL, or journal record. `WatchExecution` remains the compatible lifecycle-only stream.

`SkillSignal` carries a manager-issued signal ID, execution ID, direction, lower-case dotted name, journal sequence, and object-valued payload. The target descriptor declares accepted and emitted SIGNAL names with JSON Schemas. Direction, declaration, and payload are validated before queue or publication. `CANCEL` is manager-reserved and cannot be submitted through `SignalExecution`; call `TerminateExecution` instead.

The descriptor also exposes `shared_read` and `exclusive_use` claim templates bound to invocation fields. `ListExecutions`, `GetExecution`, and journal entries expose the resolved whole-execution claims, current `active_device_claims` for Manager-run tool calls, and an optional `queue_reason`; `claim.conflict` means the execution remains `queued` until its complete set can be acquired atomically. Compatible claim sets may run in parallel, while FIFO ordering is preserved between executions sharing a claim.

Inbound application SIGNALs may queue while the execution is `queued`, are ordered per execution, and are process-local. Identical retained request IDs return the first receipt; conflicting reuse returns `ALREADY_EXISTS`. Emitted SIGNALs, lifecycle transitions, and journal records share one replay cursor. The external Agent is the only router: EOS does not directly forward between executions and provides no topics or broadcast.

Example application SIGNAL:

```protobuf
SignalExecutionRequest {
  execution_id: "0199..."
  request_id: "egia-plan-8-signal-3"
  name: "dialogue.response"
  payload {
    fields { key: "text" value { string_value: "Hello." } }
  }
}
```

An identical retry returns the original manager-issued `signal_id`. Reusing the same request ID with a different name or payload returns `ALREADY_EXISTS`. `CANCEL` cannot be submitted through `SignalExecution`; use `TerminateExecution`. The request ID is 1–96 bytes, the payload is an object of at most 1 MiB, and at most 256 inbound application SIGNALs may be pending for one execution. The receipt contains `signal_id`, `execution_id`, the original `request_id`, and `accepted = true`; it does not assert that skill code consumed the SIGNAL.

`FollowExecutionJournal` uses `after_journal_sequence`, not lifecycle `after_sequence`. Passing zero replays the retained event history. A reconnect passes the last journal sequence whose handler completed successfully. Entries are strictly ordered per execution and contain exactly one of:

- a complete lifecycle snapshot;
- one schema-validated `SKILL_TO_AGENT` SIGNAL; or
- a `SkillJournalRecord` sourced by `SYSTEM` or `SKILL`, with kind `ACTIVITY`, `CHECKPOINT`, or `SUPPRESSION`, timestamp, elapsed time, optional duration/progress, outcome, bounded message, and sanitized fields.

The stream sends the terminal lifecycle event and then closes. Inbound SIGNALs are not echoed into the event stream; consequently the `AGENT_TO_SKILL` direction is representational and is never emitted by this stream. Unified history retains at most 1,024 events. Lifecycle append drops its oldest event at that bound, while emitted-SIGNAL and checkpoint append fail when full; automatic activity is dropped and counted. If a cursor predates the retained window, the stream returns the available suffix without a gap error.

## Global monitor and bulk termination

`MonitorExecutions` provides the `htop`-style view. Its selector accepts an optional exact skill name/version and repeated statuses; version without name is invalid. An empty status list means active executions (`QUEUED` and `RUNNING`). A new connection receives `reset = true` with all matching snapshots. A retained cursor replays later deltas; an expired cursor receives a fresh reset, while a cursor ahead of the process-local head is rejected. Updates contain a monotonically increasing `monitor_sequence`, complete `upserted` snapshots, and execution IDs removed by selector exit or retention. The bounded global history retains 4,096 updates by default.

`TerminateExecutions` is the `pkill`-style operation. It requires an exact skill name and non-empty request ID, accepts an optional exact version, and atomically selects only queued or running executions. Target IDs are sorted. Retrying the same request ID and selector returns the original target set, including an empty set; conflicting reuse returns `ALREADY_EXISTS`, and newly started matches are never added to a retry.

## End-to-end client flow

1. Call `ListSkills`, choose an exact name/version, and inspect its parameter schema, accepted/emitted SIGNALs, claim templates, and `reentrant` flag.
2. Call `StartExecution` with a stable request ID. Save both the returned `execution_id` and lifecycle `sequence`.
3. Follow lifecycle-only state with `WatchExecution(after_sequence)`, or follow lifecycle plus emitted SIGNALs with `FollowExecutionJournal(after_journal_sequence)`. Persist the matching cursor only after the event handler succeeds.
4. If the descriptor accepts an application SIGNAL, call `SignalExecution` with a new stable request ID and save the returned receipt. Retry the exact request with the same ID after an ambiguous transport failure.
5. Use `GetExecution` for one current snapshot or `ListExecutions` for the retained process view. Use `TerminateExecution` when cooperative termination is required; continue watching until a terminal snapshot arrives.

## Retention and errors

Records and idempotency keys are process-local. EOS retains at most 1,024 records and evicts eligible terminal records oldest-first. A start fails closed when capacity is exhausted and no terminal record is eligible.

| Status | Meaning |
| --- | --- |
| `INVALID_ARGUMENT` | Missing/malformed identity, arguments, request ID, explicit timeout, cursor, undeclared accepted SIGNAL name, or schema-invalid arguments/payload |
| `NOT_FOUND` | Exact skill or execution ID is not retained |
| `ALREADY_EXISTS` | A retained request ID was reused with different content |
| `FAILED_PRECONDITION` | An application SIGNAL targets a terminal execution |
| `RESOURCE_EXHAUSTED` | A record, pending SIGNAL queue, or event history is full |
| `UNAVAILABLE` | The provider cannot support the requested SIGNAL operation |
| `INTERNAL` | A runtime/provider invariant or serialization boundary failed |

Skill-level rejection and terminal execution failure are normal snapshots, not transport errors. The current gRPC listener is loopback and insecure. Remote authentication, durable executions, progress payloads, and managed or external providers are not implemented. Browser HTTP/SSE access is defined separately by the [north HTTP contract](external/http.md).

## External skill execution

EOS does not currently admit or run external skill processes. The external execution profile is a fail-closed placeholder and does not define an executable IPC contract. Author supported skills with the [built-in C++ API](skills/native/README.md); the legacy ABI v2 remains supported for existing native modules. The [local mailbox IPC guide](skills/native/ipc.md) describes cooperation within the supported skill runtime and does not grant an external process skill authority.
