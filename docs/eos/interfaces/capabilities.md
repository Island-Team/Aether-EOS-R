# CapabilityService v2

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Interfaces](README.md) / **capabilities**

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
      - **capabilities.md**
      - [contracts.md](contracts.md)
      - [grpc.md](grpc.md)
      - [capability-migration.md](capability-migration.md)
      - [skills.md](skills.md)
      - [workflow.md](workflow.md)
      - [world.md](world.md)

</details>
<!-- docs-tree:end -->

[中文](../../../docs_cn/eos/interfaces/capabilities_cn.md) · [Interface guide](README.md) · [Primitive payloads](actions.md) · [Kernel design](../design/runtime/kernel.md) · [2.0 migration](capability-migration.md)

`island.eos.v2.CapabilityService` is the public native gRPC action interface for agents. It discovers admitted **hardware capabilities** and submits their operations without requiring a process-generated device ID. EOS owns the catalog and the operation state; [Dispatcher](../design/runtime/dispatch.md) still validates and releases every primitive. Resource, model, and skill capabilities are not in this service yet. The source contract is `share/eos/contracts/eos/capabilities.proto`.

The service shares the existing `eosd` gRPC listener and its local trusted-client deployment boundary. The merged R02 listener had no per-client authentication or grant. The EOS [R03 managed admission contract](grpc.md#r03-governed-actuator-admission-target-contract) is merged and qualified: managed actuator writes require a run-scoped grant and saved active revision, and operation lookup/cancellation is role-scoped. R05/R06 and the scoped M3 exit were subsequently qualified; see the [M3 record](../../island/m3-qualification.md). The separate north HTTP adapter now exposes the same `/v2/eos` routes as REST and SSE; generated gRPC stubs remain available. The `island.eos.v1` Gateway, Skill, and Workflow services continue to serve their non-primitive operations. The five public v1 Gateway primitive action RPCs are retired in EOS 0.2.0.

## Identity and results

Device capability names are `hardware/{robot_id}/{device_class}/{part_id}/{primitive}@{version}`. Each component is percent-escaped where needed. Implicit device `motion.stop` uses the same form. Whole-runtime `robot.stop` and `safety.estop` use `runtime/{robot_id}/{primitive}@1.0.0`. The catalog rejects duplicate names at startup. The descriptor also carries logical `primitive`, `version`, and `deviceId` metadata for exact device binding without parsing the canonical name. `available` is live health for ordinary device actions; safety stop entries remain available even when devices are unhealthy. Listing a capability does not guarantee a later submission will pass Dispatcher admission.

The caller supplies an `operation_id` of 1–128 bytes. It is also Dispatcher's durable command ID and idempotency key, used by Dispatcher. Identical requests replay the same pending or terminal command; conflicting reuse returns `ALREADY_EXISTS` and never releases a second command. `Operation` deliberately contains no `capability_name`: after restart, Dispatcher can recover the command and acknowledgement by ID, but does not persist the catalog alias chosen by the caller. Keep the submitted name in client state if it is needed later.

`Operation.status` is `PENDING`, `SUCCEEDED`, `REJECTED`, `FAILED`, `CANCELLED`, `TIMED_OUT`, or `OUTCOME_UNKNOWN`. While pending, `accepted` is absent and there is no acknowledgement. A terminal result sets `accepted` and includes the canonical `primitive.ack` in `acknowledgement`. The top-level `reason_code` mirrors the acknowledgement's admission reason when it is a string; a failure after admission may have an empty top-level code and a more specific result inside the acknowledgement. `OUTCOME_UNKNOWN` must never trigger an automatic physical retry.

## RPCs

### ListCapabilities

`ListCapabilities({})` returns every admitted catalog name with current availability and its logical primitive, version, and device ID, including safety stop entries. The device ID is an EOS logical identity, not a backend endpoint. For example:

```json
{"capabilities":[{"name":"hardware/demo/base/default/base.velocity@1.0.0","available":true,"primitive":"base.velocity","version":"1.0.0","deviceId":"base-0"},{"name":"runtime/demo/robot.stop@1.0.0","available":true,"primitive":"robot.stop","version":"1.0.0"}]}
```

### SubmitOperation

`SubmitOperation` requires a nonempty `capability_name`, an explicitly present object `arguments`, and `operation_id`. Arguments are the same caller-owned object validated for that exact primitive; the capability resolves the target and version. For example:

```json
{"capabilityName":"hardware/demo/base/default/base.velocity@1.0.0","arguments":{"linear_x_m_s":0.2,"linear_y_m_s":0.0,"angular_z_rad_s":0.0,"valid_for_ms":250},"operationId":"velocity-42"}
```

Submission starts the command and may return pending immediately:

```json
{"operationId":"velocity-42","status":"OPERATION_STATUS_PENDING"}
```

A terminal response includes the unchanged dispatch acknowledgement:

```json
{"operationId":"velocity-42","status":"OPERATION_STATUS_SUCCEEDED","accepted":true,"acknowledgement":{"type":"primitive.ack","primitive":"base.velocity","primitive_version":"1.0.0","accepted":true,"reason":null,"reason_code":null,"command_id":"velocity-42","sequence":1,"result":{"status":"succeeded"}}}
```

An unknown capability returns `NOT_FOUND`; missing fields or an invalid ID return `INVALID_ARGUMENT`. Safety, health, schema, and backend outcomes follow the existing [primitive contract](actions.md). A completed semantic rejection is an OK RPC with `REJECTED` and its acknowledgement.

### GetOperation and WatchOperation

Both take `{"operationId":"velocity-42"}`. `GetOperation` returns the current retained command state. `WatchOperation` first emits that state and, if it is pending, waits for one terminal update; it has no cursor or historical replay stream. Cancelling the gRPC watch stops the watch, not the accepted command. An unknown ID returns `NOT_FOUND`. After restart, recovered prepared commands can return `OUTCOME_UNKNOWN` without being released again.

### CancelOperation

`CancelOperation` takes the same operation ID and returns its current or terminal `Operation` after asking Dispatcher to cancel. An unknown ID returns `NOT_FOUND`; non-cancellable commands follow Dispatcher's precondition failure. Cancellation targets that command only and does not stop a later command on the same device. Use an explicit stop capability where motion must be clamped.

HTTP clients use `GET /v2/eos/capabilities`, `POST /v2/eos/operations:submit`, `GET /v2/eos/operations/{id}`, `GET /v2/eos/operations/{id}:watch` (SSE), and `POST /v2/eos/operations/{id}:cancel`. The v1 primitive HTTP routes are retired in EOS 0.2.0; see the [HTTP contract](external/http.md) and [migration guide](capability-migration.md).

**Related:** [gRPC access and transport](grpc.md), [northbound runtime](../design/runtime/northbound.md), [ActionList](../design/runtime/actionlist.md).
