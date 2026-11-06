# egia.core.link

[中文](../../../docs_cn/ei/core/link.md) · [Core overview](modules.md) · [EOS 2 operation examples](../interfaces/eos.md)

Link owns one EOS gRPC client per configured stable robot alias. It publishes complete body/resource generations, reads and watches typed world resources, captures camera frames on demand, executes versioned primitives, and exposes the monitor's direct stop path. EOS drivers and providers remain private implementation details.

## Initialization and refresh

One mandatory generation consists of Gateway Discover, ListResources, and GetStatus, followed by CapabilityService.ListCapabilities. EGIA joins capabilities to the discovered robot and exact primitive/device/version bindings. A capability from another robot, a malformed identity, a duplicate descriptor, or conflicting availability fails closed. Only available capabilities joined to ready discovered bindings can execute.

For an IsOS-managed run, the three identity-bearing responses must report the same saved-graph revision and EOS projection digest, and the revision must equal the selected `egia/stages.json` revision. Missing or malformed selected revision is a startup error. The grant file delivered by IsOS must be owner-owned, mode `0600`, and exactly formed. A failed forced refresh clears write readiness before the next RPC; a later complete matching generation restores it. The saved revision, never the projection digest, accompanies governed actuator submissions.

EGIA then calls SkillService.ListSkills. Skills carry planning metadata, schemas, exact primitive references, and resource templates; native ABI and library provenance remain private to EOS. A transient unavailable skill service degrades the skill snapshot without blocking body, resource, or primitive readiness.

## Resources and camera capture

ListResources exposes the supported typed world, detection, and speech resources. WatchResource announces changed URIs; Link starts the watch before its initial read, rereads on notifications, and performs bounded periodic rereads to cover missed notifications. Only schema-valid snapshots are published. Camera frames are retrieved on demand through CaptureFrames.

## Primitive operations

Ordinary calls reach Link only after registry and safety admission. EGIA validates arguments against the exact shared action schema, requires the exact discovered device for device-scoped actions, then submits the discovered capability name. Robot-scoped capabilities use an empty device ID. Caller-provided command_id inside arguments is rejected.

EGIA creates one operation_id per admitted call. This is EOS's durable idempotency key and is echoed as primitive.ack.command_id. A pending operation has no accepted field; Link follows it with WatchOperation using the same ID until a terminal update. A result is successful only when operation status is terminal SUCCEEDED and the matching acknowledgement result status is succeeded. Operation ID, accepted value, primitive, and version must agree between the operation and acknowledgement.

Link returns rejected, failed, cancelled, timed-out, and outcome-unknown acknowledgements as non-success. A successful submission RPC alone does not mean the physical action succeeded. If the watch ends without a terminal result, or response identity is inconsistent, Link raises a transport/contract error.

Cancellation uses explicit CancelOperation with the original operation ID only for cancellable action classes. Link waits up to five seconds for a terminal cancellation boundary. It never automatically resubmits after timeout, disconnection, or OUTCOME_UNKNOWN. Fresh state reconciliation or operator intervention is required.

When a Submit reply or Watch stream is lost, Link queries GetOperation under the same grant and ID. A canonical matching terminal result settles that call; failed lookup leaves the result uncertain and fences later writes pending rediscovery. Stop and emergency use the fixed safety/monitor route; authored stage edges carry evidence and cannot command the gateway.

## Transport conditions

Before initialization, discovery-dependent properties raise rather than returning authoritative empty data. A forced initialize replaces the complete generation atomically. Unknown resources, versions, targets, cameras, and skills fail closed. gRPC status and details are preserved in link errors, source owners control stream reconnection, and closing releases the channel.

**Related:** [southbound](../southbound.md), [registry](registry.md), [safety](safety.md), and the [EOS compatibility record](../../island/eos-contracts.md).
