# `egia.core.registry`

[中文](../../../docs_cn/ei/core/registry.md) · [Core overview](modules.md)

The registry is the single in-memory catalog of executable EOS primitive bindings. Model selection is not part of execution discovery, and native skills remain a separate metadata catalog.

## Construction and replacement

Runtime construction joins canonical action schemas with the robot-scoped and device-scoped bindings embedded in `Discover`. The portable schema remains authoritative for arguments; EOS contributes the exact version, target, health, references, frame, dependencies, accepted media types, description, and numeric limits for this deployment.

The link layer retains valid degraded and unavailable bindings for diagnostics, but registry admission accepts only `health == "ready"`. Unknown primitive names or versions, missing required limits, malformed targets, and duplicate compound identities fail closed. Per-backend rediscovery atomically replaces that alias while preserving other robot aliases; stale and current generations are never merged.

`PrimitiveRegistry.from_contracts` is a deterministic hardware-free fixture helper. Runtime composition uses discovered bindings and never invents a device or primitive from the canonical schema alone.

## Primitive identity and visibility

Executable identity is `{name, exact SemVer, robot alias, scope, device target}`. Device-scoped calls require an explicit device ID. Robot-scoped `robot.stop` and `safety.estop` have no device target. Resolution never infers a target when more than one current candidate exists.

A descriptor also records management `binding_kind`, health, supported `references`, `absolute_frame`, dependency health, `accepted_media_types`, deployment description, canonical argument schema, and enforced numeric `limits`. The northbound catalog projects these fields unchanged.

Planner rendering emits compact target-aware signatures such as:

```text
gripper.motion@robot-a/gripper-0 action=<close|open> [force_n=<number[0..10]>]
```

Operator-tier arm primitives are included only when `[primitives].expose_operator = true`; they never become planner-visible. Privileged stop paths remain monitor/operator controlled, and only canonical v3 names enter the catalog.

## Native-skill catalog

`SkillService.ListSkills` supplies sorted descriptors with exact identity, description, schemas, exact primitive references, and resource templates. Transient unavailability replaces stale claims with a degraded snapshot; malformed metadata is an incompatible contract. A skill composed from planner-hidden primitives is omitted from planner guidance.

Skills describe possible composition but never enter the executable primitive registry. EGIA exposes their metadata on `/skills` and does not provide a skill execution route or call EOS skill lifecycle RPCs.

## Consumers

- decision renders planner-visible signatures and exact targets;
- safety resolves the binding before governed egress;
- coordination bids only for ready primitives assigned to a current robot/device;
- northbound publishes `/primitives` and metadata-only `/skills` snapshots;
- context renders concise primitive and skill guidance.

## Resolution conditions

Exact current target resolution succeeds once. Unknown, unavailable, duplicate, ambiguous, or version-mismatched bindings fail closed. Caller-supplied `arguments.command_id` is not an argument and is rejected before EOS egress. Refresh preserves no stale callable entries.

**Related:** [link](link.md), [decision](decision.md), [safety](safety.md), and [northbound](../northbound.md).
