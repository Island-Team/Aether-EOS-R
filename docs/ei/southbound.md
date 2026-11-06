# Southbound EOS interface

[中文](../../docs_cn/ei/southbound.md) · [Body](southbound/body.md) · [World](southbound/world.md) · [Primitives](southbound/primitives.md) · [Skills](southbound/skills.md) · [EOS 2 wire examples](../ei/interfaces/eos.md)

EGIA consumes EOS Gateway discovery and resources, the EOS 2 CapabilityService operation API, and SkillService over native gRPC. EOS owns protobufs and JSON Schemas selected through collision-checked shared resource roots. EGIA owns generated Python protobuf bindings, typed adapters, safety policy, cognition, and operator-facing projections. The EOS HTTP/ProtoJSON listener is not used by EGIA.

The generated contract is pinned to Island-eos commit c43e5a32eafd9bf22b2dba2940662dcded60111c. This is a development commit, not an EOS release tag. See [EOS compatibility](../island/eos-contracts.md) for the pin and [operation examples](interfaces/eos.md) for the exact wire flow.

Startup requires Gateway discovery, resources, status, capabilities, and the canonical schema inventory. EGIA validates each capability against the discovered robot and exact primitive/device/version binding. A peer with malformed or cross-robot capabilities is rejected; schemas alone never create executable permission.

## Data flow

\`\`\`mermaid
flowchart LR
  discover["Gateway discovery"] --> generation["Atomic robot/device generation"]
  capabilities["CapabilityService catalog"] --> binding["Exact ready capability join"]
  generation --> binding
  resources["Read/watch resources"] --> typed["Canonical schema validation"]
  binding --> registry["Executable registry"]
  typed --> perception["World + detection + ASR admission"]
  registry --> safety["EGIA safety gate"]
  safety --> operation["SubmitOperation with operation_id"]
  operation --> terminal["WatchOperation terminal acknowledgement"]
  interrupt["Timeout or cancellation"] --> cancel["CancelOperation by operation_id"]
  cancel --> terminal
  terminal --> mission["Mission outcome + structured log"]
\`\`\`

Every executable binding must be healthy, match the installed schema at its exact version, and pass local policy. Unhealthy bindings remain visible for diagnostics but cannot execute. Refresh replaces the full generation atomically.

## Interface groups

- [Body](southbound/body.md) covers robot/device identity, state, health, binding scope, rediscovery, and status.
- [World](southbound/world.md) covers typed world state, detection, speech, resource watches, and camera capture.
- [Primitives](southbound/primitives.md) covers action versions, durable operation identity, terminal outcomes, explicit cancellation, and uncertain results.
- [Skills](southbound/skills.md) covers descriptors and asynchronous execution through the dedicated skill service.

## Contract ownership and compatibility

EOS owns the proto and JSON Schema sources. scripts/generate_eos_proto.py generates EGIA's checked Python projection from the pinned commit; generated files are not edited manually. A contract change updates EOS first, records its exact commit, regenerates bindings, then updates typed consumption, tests, and both language docs. See [contract validation](../island/contracts.md).
