# EOS 2 capability operations

[中文](../../../docs_cn/ei/interfaces/eos.md) · [Link transport](../core/link.md) · [Shared EOS contract](../../island/eos-contracts.md)

EGIA pins and generates its EOS protobuf bindings from Island-eos commit c43e5a32eafd9bf22b2dba2940662dcded60111c. This is a development contract commit, not a public EOS release tag. Regenerate with scripts/generate_eos_proto.py after changing the pin; do not edit generated files by hand.

## Capability identity and admission

EGIA first discovers the robot, then lists capabilities. A capability must name that discovered robot and match its descriptor's primitive and version. EGIA only binds a capability to a primitive already present in EOS discovery, at the exact device and version; advertised aliases do not create new permission. The accepted name forms are:

```text
hardware/{robot_id}/{device_class}/{part_id}/{primitive}@{version}
runtime/{robot_id}/{primitive}@1.0.0
```

For example:

```text
hardware/robot-123/base/base-0/base.velocity@1.0.0
runtime/robot-123/robot.stop@1.0.0
```

The first is a device capability. The runtime form is process-wide and has an empty device_id. A catalog entry for a different robot is a malformed peer response and fails discovery. If multiple names describe the same action target, EGIA accepts them only when availability agrees and selects one deterministically.

## Operation wire exchange

operation_id is the durable idempotency key. EGIA creates one UUID per admitted call and sends it in SubmitOperationRequest; a retry must never mint a new ID for the same physical action. The capability name is the exact admitted descriptor, and the arguments are a protobuf Struct validated against the matching shared action schema before transport.

Request:

```json
{
  "capability_name": "hardware/robot-123/base/base-0/base.velocity@1.0.0",
  "arguments": {
    "linear_x_m_s": 0.1,
    "linear_y_m_s": 0.0,
    "angular_z_rad_s": 0.0,
    "valid_for_ms": 100
  },
  "operation_id": "75a852c0-894a-4d93-b1b9-31fd642621cd"
}
```

A pending operation has no accepted field and no terminal acknowledgement:

```json
{
  "operation_id": "75a852c0-894a-4d93-b1b9-31fd642621cd",
  "status": "OPERATION_STATUS_PENDING"
}
```

EGIA follows the same operation ID with WatchOperation until a terminal update. A successful terminal response carries both the admission decision and the canonical primitive.ack:

```json
{
  "operation_id": "75a852c0-894a-4d93-b1b9-31fd642621cd",
  "status": "OPERATION_STATUS_SUCCEEDED",
  "accepted": true,
  "acknowledgement": {
    "type": "primitive.ack",
    "primitive": "base.velocity",
    "primitive_version": "1.0.0",
    "accepted": true,
    "reason": null,
    "reason_code": null,
    "command_id": "75a852c0-894a-4d93-b1b9-31fd642621cd",
    "sequence": 42,
    "result": {"status": "succeeded"}
  }
}
```

Only terminal SUCCEEDED with an acknowledgement result of succeeded, matching operation ID, primitive, version, and accepted value is success. A terminal status and acknowledgement that disagree are a protocol error. Rejected, failed, cancelled, timed out, and outcome-unknown results are not success.

## Cancellation and uncertain outcomes

Cancellation is an explicit CancelOperation request using the same durable operation ID:

```json
{"operation_id": "75a852c0-894a-4d93-b1b9-31fd642621cd"}
```

EGIA only requests cancellation for action classes EOS declares cancellable, and then watches for the terminal boundary. An interrupted or timed-out non-cancellable action is not replayed. OUTCOME_UNKNOWN is terminal for the current observation and must never trigger an automatic submit retry; reconcile from fresh state or ask the operator.

The transport deliberately does not interpret a successful gRPC submission as successful physical completion. A response must be terminal, or the watch stream must yield a matching terminal operation. Operation identity is checked on every response and update.

## Verification

The stubbed contract tests cover robot identity, exact admitted bindings, command identity, pending-to-terminal flow, cancellation, malformed acknowledgements, and non-retry of uncertain outcomes. Public release additionally requires a real EOS process integration test against an exact public EOS release.
