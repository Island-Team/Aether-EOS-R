# Migrate action clients to EOS capabilities

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Interfaces](README.md) / **capability migration**

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [eos/](../README.md)
    - [interfaces/](README.md)
      - [external/](external/README.md)
      - [skills/](skills/README.md)
      - [southbound/](southbound/README.md)
      - [actions.md](actions.md)
      - [body.md](body.md)
      - [capabilities.md](capabilities.md)
      - [contracts.md](contracts.md)
      - [grpc.md](grpc.md)
      - **capability-migration.md**
      - [skills.md](skills.md)
      - [workflow.md](workflow.md)
      - [world.md](world.md)

</details>
<!-- docs-tree:end -->

[中文](../../../docs_cn/eos/interfaces/capability-migration_cn.md) · [CapabilityService](capabilities.md) · [HTTP adapter](external/http.md) · [WorkflowService](workflow.md)

EOS 0.2.0 retires the five public `GatewayService` primitive action RPCs: `CallPrimitive`, `StartPrimitive`, `GetPrimitive`, `WatchPrimitive`, and `CancelPrimitive`. The HTTP adapter also retires `POST /v1/eos/primitives:call`, `POST /v1/eos/primitives:start`, `GET /v1/eos/primitives/{command_id}`, and `POST /v1/eos/motions:cancel`. Other v1 Gateway, Skill, and Workflow operations remain available. These retired routes return no action result; update clients before deploying 1.0.0.

| Earlier call | EOS 0.2.0 call |
| --- | --- |
| `Discover` primitive binding, then `CallPrimitive` or `StartPrimitive` with name, version, and device ID | `ListCapabilities`, select one canonical name, then `SubmitOperation` with `capabilityName`, object `arguments`, and `operationId` |
| `GetPrimitive(command_id)` | `GetOperation(operation_id)` |
| `WatchPrimitive(command_id)` | `WatchOperation(operation_id)`; HTTP uses `GET /v2/eos/operations/{id}:watch` SSE |
| `CancelPrimitive(command_id)` | `CancelOperation(operation_id)`; HTTP uses `POST /v2/eos/operations/{id}:cancel` |

The operation ID is the Dispatcher command ID and idempotency key. Preserve an existing command ID when migrating an in-flight command; do not submit the same physical intent under a new ID to work around an uncertain outcome. Submission can return `PENDING`; read or watch until terminal. A terminal `Operation` includes the existing `primitive.ack`, including admission and physical outcome. `OUTCOME_UNKNOWN` never authorizes automatic release again. A watch disconnect only closes that watch. Use an explicit stop capability when motion must stop.

For HTTP, discover with `GET /v2/eos/capabilities`, then submit a selected name:

```sh
curl -sS -H 'Content-Type: application/json' \
  -d '{"capabilityName":"hardware/demo/base/default/base.velocity@1.0.0","operationId":"move-42","arguments":{"linear_x_m_s":0.1,"linear_y_m_s":0,"angular_z_rad_s":0,"valid_for_ms":250}}' \
  http://127.0.0.1:16201/v2/eos/operations:submit
```

Treat the canonical name as an opaque identity. `ListCapabilities` also returns logical primitive, version, and device ID metadata for exact selection. Availability is a live hint; Dispatcher may still reject a later command. Keep the chosen capability name in client state when needed because a recovered operation retains its ID and outcome but cannot reconstruct that alias.

For new ActionLists, use `WorkflowAction.capability` with `capabilityName` instead of `WorkflowAction.primitive` with name, version, and device ID. The historical primitive field remains readable and executable for stored definitions and journals; no automatic rewrite of durable workflow data occurs. Skill targets and workflow service version remain unchanged. In the REPL, use `capabilities`, `call CAPABILITY`, `operation ID|last`, `watch-operation ID|last`, and `cancel-operation ID|last`; the old primitive `call` and `cancel-motion` commands are retired.

EOS 0.2.0 is the product release version. Primitive schema versions such as `base.velocity@1.0.0`, skill package versions, and the internal worker IPC protocol version are independent and are not bumped by this migration. Native skill ABI v2 remains supported, including the additive v2.1 callbacks; v1 skill ABI remains unsupported.
