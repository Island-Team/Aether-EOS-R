# EOS 2 capability operations

[中文](../../../docs_cn/ei/interfaces/eos-v2.md) · [Link transport](../core/link.md) · [Shared EOS contract](../../island/eos-contracts.md)

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

### Managed IsOS generation and grant

For a supervised managed run, IsOS passes only `EGIA_EOS_CONTROL_GRANT_FILE` to EGIA. The value is an absolute path to an owner-owned regular file with mode `0600`, containing exactly 64 lowercase hexadecimal digits and one newline. EGIA never logs the grant. IsOS-rendered `egia/stages.json` carries the selected saved graph `revision` as exactly 64 lowercase hexadecimal digits. EGIA requires matching `eos-active-revision` and `eos-projection-sha256` gRPC response metadata on Discover, GetStatus, and ListCapabilities **and compares the active revision with that selected overlay revision** before publishing a generation. Both metadata values are lowercase SHA-256 digests; the first identifies IsOS's saved whole graph and is the only value accepted in `x-eos-active-revision`. The projection digest is diagnostic and cannot replace it. A mixed, missing, malformed or consistently wrong EOS generation, an invalid selected revision, or unsafe grant file disables writes until complete fresh matching discovery succeeds. A forced refresh fences the old generation before its first RPC.

EGIA sends `authorization: Bearer <grant>` on SubmitOperation, GetOperation, WatchOperation, and CancelOperation. Movement, `motion.stop`, and `robot.stop` Submit also carry `x-eos-active-revision: <saved-graph-revision>`; authenticated `safety.estop` may omit it. Lookup, watch, and cancellation use the same durable role and operation ID and do not require a current revision. The fixed decision → safety → link and monitor emergency paths are code-owned; authored graph edges cannot submit an EOS command or bypass EOS Dispatcher admission. A standalone EOS without managed metadata remains a read-only discovery surface for managed EGIA.

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

If SubmitOperation loses its reply or WatchOperation disconnects, EGIA asks authenticated GetOperation for the **same ID**. A matching terminal acknowledgement can settle the call; an absent, sealed, unauthorized, pending-without-terminal or mismatched result remains uncertain. EGIA never submits that physical action a second time. A transport break invalidates the published generation, so another write requires fresh discovery.

The transport deliberately does not interpret a successful gRPC submission as successful physical completion. A response must be terminal, or the watch stream must yield a matching terminal operation. Operation identity is checked on every response and update.

## Verification

The stubbed contract tests cover robot identity, exact admitted bindings, command identity, pending-to-terminal flow, cancellation, malformed acknowledgements, and non-retry of uncertain outcomes. Public release additionally requires a real EOS process integration test against an exact public EOS release.
