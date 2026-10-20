# WorkflowService gRPC interface

[Northbound interfaces](README.md) · [ActionList runtime](../../design/runtime/actionlist.md) · [SkillService](skills.md) · [Primitive calls](actions.md) · [中文](../../../../docs_cn/eos/develop/interfaces/workflow_cn.md)

`island.eos.v1.WorkflowService` is the native gRPC API for submitting, inspecting, snapshotting, and following bounded action lists. The Agent chooses the complete plan. EOS validates its dependency graph and mechanically starts every eligible action; it is not a planner and does not invent a next action.

The service shares the `eosd` gRPC listener with `GatewayService` and `SkillService`. This page defines the gRPC contract only.

## RPC inventory

| RPC | Request | Response | Purpose |
| --- | --- | --- | --- |
| `ListWorkflowTemplates` | `ListWorkflowTemplatesRequest` | `ListWorkflowTemplatesResponse` | Discover deployment-registered workflow templates |
| `GetWorkflowTemplate` | `GetWorkflowTemplateRequest` | `WorkflowTemplateDescriptor` | Read one template by name |
| `RunWorkflowTemplate` | `RunWorkflowTemplateRequest` | `ActionList` | Copy, validate, and begin a registered template |
| `ApplyActionList` | `ApplyActionListRequest` | `ActionList` | Validate, accept, and begin a plan |
| `GetActionList` | `GetActionListRequest` | `ActionList` | Read one plan by `workflow_id` |
| `ListActionLists` | `ListActionListsRequest` | `ListActionListsResponse` | Read retained plans |
| `GetRuntimeSnapshot` | `GetRuntimeSnapshotRequest` | `ManagerRuntimeSnapshot` | Bootstrap executions, facts, plans, and an event cursor |
| `WatchActionListEvents` | `WatchActionListEventsRequest` | stream `ActionListEvent` | Replay and follow ActionList and accepted-fact mutations |

Unary requests use the default protobuf instance when empty. Send an optional `x-request-id` metadata value using 1–96 ASCII letters, digits, `.`, `_`, `:`, or `-` for log correlation. Call deadlines and cancellation use normal gRPC semantics. Disconnecting `WatchActionListEvents` stops only that stream and does not cancel actions.

## Registered templates

At the EOS startup barrier, every `*.json` file under the deployment's `share/workflows` directory is parsed as a `WorkflowDefinition`. The filename stem is its catalog name. A malformed template prevents EOS from becoming ready, matching the fail-fast behavior of other deployment-owned runtime catalogs.

`ListWorkflowTemplates` and `GetWorkflowTemplate` are discovery operations and do not create ActionLists. `RunWorkflowTemplate` copies the registered definition and submits it through the same ActionList Manager path as `ApplyActionList`. A supplied `workflow_id` replaces the template's authoring ID; otherwise EOS appends a fresh UUID. The stored template remains immutable, while facts, dependencies, concurrency, supersession, and execution outcomes remain properties of the newly created ActionList instance.

## Applying a definition

`ApplyActionListRequest.definition` is required:

| Field | Type | Rules |
| --- | --- | --- |
| `workflow_id` | string | Required and non-empty |
| `actions` | repeated `WorkflowAction` | Required, non-empty, and unique non-empty `action_id` values |
| `supersedes_workflow_ids` | repeated string | Existing eligible lists to retire; self-reference and duplicates are invalid |

Each `WorkflowAction` contains:

| Field | Type | Rules |
| --- | --- | --- |
| `action_id` | string | Unique inside the definition |
| `skill` | `SkillReference` | Exactly one target; exact `name` and `version` |
| `primitive` | `PrimitiveActionTarget` | Exactly one target; exact `name`, `version`, and `device_id` |
| `control` | `ActionControlTarget` | Exactly one target; terminate another action in this list |
| `noop` | `NoopActionTarget` | Exactly one target; succeed without an external side effect |
| `parameters` | `google.protobuf.Struct` | Skill or primitive arguments validated by the selected owner |
| `depends_on` | repeated string | Every ID must exist, may appear before or after its action, and the complete graph must be acyclic |
| `when` | `StateCondition` | Optional latest-fact or detection-resource equality guard |
| `output_bindings` | repeated `ActionOutputBinding` | Explicit same-list copies from earlier successful action outputs into this action's parameter template |
| `choice_group` | string | Optional mutually exclusive branch group; requires `when` |
| `execution_mode` | `ActionExecutionMode` | Optional `TASK` or `MONITOR`; unspecified behaves as a normal task |
| `monitor_lifetime` | `MonitorLifetime` | Required for monitors: `EXPLICIT` or `WORKFLOW` |
| `finalizer` | bool | Runs after all dependencies become terminal, including after failure or supersession |
| `continue_on_failure` | bool | Tolerates this action's terminal failure and allows unbound dependents to continue; output bindings still require success and supersession still cancels normal work |

ProtoJSON workflow templates spell `continue_on_failure` as `continueOnFailure`.

A fact condition requires `fact_key`; optional `source_action_id` restricts it to the execution created by that action. A resource condition instead requires `resource_uri`, `detection_kind` (`human` or `object`), and an allowlisted `detection_value_pointer` evaluated against each detection in the latest resource snapshot. The two condition forms are mutually exclusive. `equals` uses protobuf `Value` equality. Optional non-negative `max_age` applies to the fact event or resource observation time. Age is reevaluated on a later scheduling event; expiration alone does not create a timer or revision.

```text
definition {
  workflow_id: "greet-visitor-42"
  actions {
    action_id: "observe"
    skill { name: "observe_human" version: "1.0.0" }
    parameters { fields { key: "robot_name" value { string_value: "robot-0" } } }
  }
  actions {
    action_id: "greet"
    skill { name: "speak" version: "1.0.0" }
    depends_on: "observe"
    when {
      fact_key: "human.present"
      equals { bool_value: true }
      max_age { seconds: 5 }
    }
  }
}
```

Each binding contains `source_action_id`, `source_output_pointer`, and `destination_parameter_pointer`. Both pointers follow RFC 6901, including the empty pointer for the complete value and `~0`/`~1` escaping. The source must be an earlier action in the same definition. Its normalized `output` is the direct skill result or the primitive acknowledgement's `result` member. The destination must already identify a location in the object-valued submitted `parameters`; bindings may replace objects, arrays, scalars, or null values. Exact or ancestor/descendant destination overlaps are conflicts. A binding implicitly adds its source to `depends_on`.

Actions sharing a non-empty `choice_group` are mutually exclusive. The first eligible member is selected and every still-blocked/ready sibling becomes `SKIPPED`; each member therefore requires its own condition. A no-op is useful for an explicit branch with no external effect. A control action currently supports only `TERMINATE` and targets a different action in the same list.

A `MONITOR` skill or primitive is considered dependency-ready once queued or running; output bindings still require actual success. `EXPLICIT` monitors run until a control action terminates them. `WORKFLOW` monitors are cooperatively stopped after all non-workflow-monitor actions finish, and are also stopped on workflow failure. A requested monitor stop maps cancellation to the successful `STOPPED` action state. Control and no-op actions cannot be monitors.

Structural validation is atomic: malformed actions, duplicate IDs, unknown or self dependencies/control targets/condition sources, cycles, missing or future binding sources, invalid pointers, missing destination paths, conflicting destinations, invalid condition shapes, invalid choice groups, and invalid monitor declarations reject the definition before it is stored. Source output paths are checked when the source has succeeded. A missing runtime path rejects the destination without dispatch. Target identity and resolved-parameter validation occurs through the owning SkillManager or Dispatcher boundary when an eligible action is admitted there; target rejection is retained as that action's terminal `REJECTED` result.

## Scheduling and state

An action is eligible when every ordinary dependency has `SUCCEEDED`, every monitor dependency is active or successfully stopped, and its optional condition matches the latest sufficiently fresh fact or resource snapshot. All eligible actions may start concurrently, subject to SkillManager claims and Dispatcher admission. Skill targets create a `SkillExecution`; primitive targets receive an EOS-generated `command_id`. Array position has no scheduling meaning except that output-binding sources must precede their destinations.

`ActionItem` reports the submitted `action`, `status`, optional `execution_id` or `command_id`, terminal legacy `result`, normalized JSON-valued `output`, immutable `resolved_parameters` once dispatch is attempted, stable machine-readable `reason`, and optional human-readable `message`. Resolution occurs exactly once immediately before dispatch; `action.parameters` remains the submitted template. Action states are `BLOCKED`, `READY`, `QUEUED`, `RUNNING`, `SUCCEEDED`, `REJECTED`, `FAILED`, `CANCELLED`, `TIMED_OUT`, `OUTCOME_UNKNOWN`, `SKIPPED`, and `STOPPED`. `SKIPPED` means another member of the choice group was selected; `STOPPED` means a monitor reached its declared stop point successfully.

`ActionList` contains `workflow_id`, monotonically increasing `revision`, derived `status`, the complete item array, creation/update timestamps, supersession inputs, and optional `superseded_by_workflow_id`. List states are `PENDING`, `RUNNING`, `SUCCEEDED`, `FAILED`, `SUPERSEDING`, and `SUPERSEDED`. Clients replace a cached ActionList with a newer complete revision rather than merging item arrays.

Any unsuccessful terminal action makes the list `FAILED` and prevents later blocked or ready siblings from dispatching. Already running siblings are not automatically cancelled. Supersession requests cooperative cancellation, prevents new work on the old list, retains both lists, and eventually moves the old list to `SUPERSEDED`. EOS never retries a rejected, failed, cancelled, timed-out, or outcome-unknown action.

## Events and cursors

`ActionListCursor` contains opaque boot `epoch` and unsigned `sequence`. EOS generates a new epoch each time the ActionList manager is constructed. Sequence starts at zero in an empty snapshot and increases by one for every retained `ActionListEvent` in that epoch. A cursor identifies the last mutation already incorporated by the client; replay is exclusive and returns only events with greater sequence.

`ActionListEvent` contains `cursor`, server `observed_at`, and exactly one payload:

- `action_list`: a complete `ActionList` revision after an admitted, replaced, superseded, dispatched, progressed, or terminal mutation; replace the cached object for its `workflow_id`.
- `fact`: the complete accepted `SystemStateFact` for its key; replace the cached latest fact for that key.

Rejected stale or duplicate facts do not produce events. Event payloads are complete state projections, not patches. The stream interleaves both payload kinds in one total ActionList-domain sequence.

The core retains the latest 4,096 events across both payload kinds by default. Retention is in memory and bounded by count, not time. High mutation volume can expire a disconnected client's cursor even when the EOS process has not restarted.

`WatchActionListEventsRequest.after` is required and its `epoch` must be non-empty. The server first replays retained events with sequence greater than `after.sequence`, then waits for live events. Use only cursors issued by EOS; a same-epoch sequence greater than the latest published sequence, including `uint64` maximum, is rejected as `FAILED_PRECONDITION` rather than waiting or overflowing.

## Runtime snapshot and atomicity

`ManagerRuntimeSnapshot` contains `captured_at`, retained `SkillExecution`s, latest `SystemStateFact`s, retained `ActionList`s, and `action_list_cursor`. The facts, ActionLists, and `action_list_cursor` are copied under the same ActionList state lock. Therefore every ActionList/fact mutation incorporated by that snapshot has sequence at or below the cursor, and every later mutation is available through the event path unless retention expires.

Skill executions come from SkillManager before the ActionList state copy, so the aggregate snapshot is not an atomic transaction across SkillManager and ActionList. Use execution sequences for execution freshness and use `action_list_cursor` only for ActionList/fact recovery.

`SystemStateFact` exposes `key`, JSON-compatible `value`, `source`, `source_execution_id`, `source_sequence`, `event_time`, and `received_time`. The current runtime mirrors `source` into `source_execution_id`; `frame_id`, `unit`, `confidence`, and `validity` remain at protobuf defaults and must not drive decisions.

## Resnapshot and resume algorithm

Use this sequence for initial connection and every recovery:

1. Call `GetRuntimeSnapshot` and replace the local retained ActionList and latest-fact views with the complete response.
2. Store `action_list_cursor` only after the snapshot has been applied locally.
3. Open `WatchActionListEvents` with that cursor in `after`; mutations committed after the atomic ActionList snapshot copy replay without a gap.
4. Apply each complete payload in stream order, then advance the stored cursor to that event's `cursor`. Persist or acknowledge a cursor only after its payload is durably applied by the client.
5. After an ordinary disconnect or deadline, reconnect using the last applied cursor. Exclusive replay avoids resending that event.
6. On `FAILED_PRECONDITION` or `OUT_OF_RANGE`, discard the incremental assumption, obtain a fresh `GetRuntimeSnapshot`, replace local state, and resume from its new cursor.

Clients should verify one epoch and strictly increasing sequence within a stream. A repeated event can safely replace the same complete cached object, but clients must not infer missing events from ActionList revision alone because fact events share the sequence.

## Errors and client behavior

The primary statuses are:

- `INVALID_ARGUMENT`: malformed definition/request, missing `workflow_id`, missing `after`, or empty cursor epoch.
- `NOT_FOUND`: requested `workflow_id` does not exist.
- `FAILED_PRECONDITION`: invalid supersession/replacement state, cursor epoch differs from this EOS boot, or same-epoch cursor sequence is in the future.
- `OUT_OF_RANGE`: cursor sequence is older than the retained event window.
- `UNAVAILABLE`: a required composed runtime dependency is unavailable.
- `INTERNAL`: conversion failure, broken invariant, or unexpected exception.

Accepted target failures remain successful typed `ActionList`/`ActionListEvent` payloads rather than transport failures. A missing workflow after restart means process-local state was lost; it does not prove physical effects were reversed. Stop following a terminal list only if no other retained workflows or facts are of interest; `WatchActionListEvents` is a manager-wide stream and does not close when one workflow terminates.

## Logging and debugging

Every RPC runs in the standard correlation scope. The `northbound` logger records `service: "WorkflowService"`, method, final gRPC status, and total duration as `grpc_request_completed` or `grpc_request_rejected`; it does not log definitions, ActionLists, facts, event payloads, or raw protobuf. A long-lived watch emits its request completion record when the stream closes.

Domain mutations remain visible through the payload-free `actionlist` logger. For recovery failures, record the method, returned gRPC status, cursor epoch, and sequence in the client; EOS logs intentionally avoid duplicating cursor or payload data on every streamed event. Normal empty 250 ms follow waits are silent.

## Testing

`eos_actionlist_tests` covers initial and monotonic cursors, complete ActionList and fact events, snapshot/cursor consistency, exclusive replay, live fact follow, bounded-retention expiry, epoch mismatch, and future-cursor rejection in addition to DAG scheduling behavior.

Northbound integration tests cover protobuf conversion, live `WatchActionListEvents` delivery after a snapshot cursor, stream cancellation, `FAILED_PRECONDITION` epoch mismatch, `GetRuntimeSnapshot.action_list_cursor`, health, and reflection. Contract-interface tests verify the server-streaming RPC remains generated from `workflow.proto`; core unit tests remain authoritative for expired and future cursors.
