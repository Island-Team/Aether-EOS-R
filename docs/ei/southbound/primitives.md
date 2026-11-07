# Primitive operation contract

[中文](../../../docs_cn/ei/southbound/primitives.md) · [Southbound overview](../southbound.md) · [Wire examples](../interfaces/eos.md)

EGIA owns command identity and only executes after registry and safety admission. It rejects caller-supplied arguments.command_id, creates one operation ID per admitted call, and submits it with the exact admitted capability name. The operation ID is durable and is also echoed as primitive.ack.command_id.

The capability is joined to Gateway discovery by robot ID, primitive, exact version, and device ID. Schemas describe valid arguments; they do not independently admit actions. Device-scoped calls require the exact discovered device, while runtime-scoped actions have an empty device ID.

SubmitOperation may return a pending operation without accepted or an acknowledgement. EGIA then watches using the same operation ID. Only terminal SUCCEEDED paired with a matching acknowledgement whose result.status is succeeded counts as success. Rejected, failed, cancelled, timed_out, and outcome_unknown are non-success. Pending, mismatched identities, or disagreement between terminal status and acknowledgement are never treated as success.

EGIA bounds submission to five seconds and terminal watching to thirty seconds. Lost or timed-out responses are uncertain: the client never submits the same action again automatically and never generates a replacement ID for it. OUTCOME_UNKNOWN is terminal for that observation and also never triggers a retry. Reconcile from fresh state or ask an operator.

Cancellation is explicit CancelOperation using the original ID, only for EOS-declared cancellable action kinds; EGIA then waits up to five seconds for the terminal cancellation result. Cancelling the local gRPC await does not itself stop physical work.

**Related:** [EOS wire exchange](../interfaces/eos.md), [safety](../core/safety.md), and [mission recovery](../core/missions.md).
