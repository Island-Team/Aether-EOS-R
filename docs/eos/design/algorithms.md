# Algorithm and Edge Optimization Notes

This document expands the research and implementation directions introduced in the [EOS technical solution](eos-2607.md). It is a roadmap, not a claim that every algorithm below is implemented. Each section states its present maturity and the evidence required before it may control real hardware.

## Agent-owned skill library

**Maturity: native execution foundation and asynchronous lifecycle implemented; policy remains Agent-owned.** EOS can load trusted deployment-owned code libraries, discover exact skill identities, and execute them through the dedicated asynchronous gRPC `SkillService`. The only host capabilities given to native code are validated primitive calls and declared resource reads. Future Agent policy may compose that foundation with:

- versioned input, output, and state contracts;
- required device classes, feeds, resources, and model artifacts;
- timing budget, supported rate, freshness limit, and cancellation behavior;
- deterministic preconditions and safety envelope;
- lifecycle state: `discovered → admitted → ready → active → draining → offline`;
- health, provenance, implementation identity, and compatibility range.

Selection should be deterministic given the same registry snapshot. For a request `q` and admitted candidates `s`, filter hard constraints first, then rank viable candidates with an explainable score such as

```text
score(s | q) = capability_match
             - latency_cost
             - resource_pressure
             - uncertainty_penalty
             - switching_cost
```

The exact weights are Agent policy, but every term should come from observable state. A model may recommend a candidate, but selection, lifecycle policy, memory, and arbitration remain Agent responsibilities. EOS loads only explicitly configured native libraries and enforces each embedded primitive allowlist, canonical argument validation, device admission, targeting, the base-velocity lease, stop, and estop. The unified SkillManager currently supports only a bounded ActionList plan with simple dependencies and latest-fact equality conditions. There is no general workflow DSL, durable workflow state, or persisted semantic skill registry.

## Dynamic low-level reflexes

**Maturity: research direction; classical baseline first.** A reflex consumes fresh local signals and proposes a bounded correction faster than deliberative planning. Reflexes should be composable declarations rather than hard-wired backend branches: trigger, required feeds, target device class, priority, maximum authority, expiry, and recovery condition.

At cycle `t`, the reflex host can combine a nominal command `u_nom`, classical corrections `r_i`, and an optional learned residual `r_nn`:

```text
u_candidate(t) = u_nom(t) + sum(clamp(r_i(t), envelope_i))
u_reflex(t)    = clamp(u_candidate(t) + alpha * r_nn(t), reflex_envelope)
u_release(t)   = hard_gate(u_reflex(t), state(t), limits, watchdogs)
```

`alpha` is zero unless the model artifact, input freshness, confidence, and operating domain are admitted. The learned residual cannot widen the classical envelope. Conflicting reflexes resolve by safety priority and declared resource ownership, not arrival timing. Emergency stop and watchdog expiry always dominate arbitration.

Promotion requires worst-case execution measurements, stale-input tests, conflict tests, deterministic fallbacks, and hardware-in-the-loop fault injection. Learned reflexes additionally require an out-of-distribution gate, artifact provenance, shadow-mode evaluation, and rollback.

## Shared perception representation

**Maturity: research direction.** Multiple consumers often repeat the same image/audio encoder and move the same tensors through LPDDR. Where tasks and model families are compatible, EOS should host one compact temporal encoder and publish a bounded representation `z_t` to task heads:

```text
z_t = encoder(frame_t, cache(z_{t-1}))
detections_t = detection_head(z_t)
tracks_t      = tracking_head(z_t, tracks_{t-1})
scene_delta_t = scene_head(z_t)
policy_hint_t = policy_adapter(z_t)
```

“Shared tokens” means a versioned local latent/token buffer with explicit shape, quantization, timestamp, coordinate frame, producer identity, and compatibility hash. It is not a universal embedding contract. Consumers must reject incompatible or stale representations and fall back to geometric feeds or their own encoder when sharing would reduce correctness.

Evaluation must compare the shared pipeline with independent models on task quality, end-to-end latency, peak memory, LPDDR traffic, energy, and failure coupling. Sharing is accepted only when the total system improves without silently tying unrelated safety domains together.

## Freshness and multi-rate exchange

**Maturity: partial foundation.** EOS already exposes versioned snapshots and backend feed-rate envelopes. The target runtime extends this with fixed-capacity single-writer slots between rate domains. Each value carries at least `sequence`, monotonic timestamp, validity interval, source health, and schema or layout identity.

A consumer at time `t` accepts a sample produced at `t_p` only when:

```text
t >= t_p
t - t_p <= max_age
source_health == ready
layout_id is compatible
```

No faster loop waits for a slower producer. On expiry it applies its declared degradation: reuse within a bounded hold window, switch to a lower-capability skill, clamp motion, or stop. The response must be testable without the producer present.

## LPDDR-aware edge execution

**Maturity: measurement and design direction.** On unified-memory edge platforms, CPU, GPU, NPU, camera, and display may compete for one LPDDR bus. Nominal accelerator utilization is therefore not a sufficient optimization target. Approximate steady traffic for feed `i` as:

```text
bandwidth_i = payload_bytes_i * rate_i * effective_copy_count_i
bandwidth_total = sum(bandwidth_i) + model_weights + intermediate_tensors
```

The engineering sequence is:

1. Measure payload size, rate, copies, cache misses, accelerator transfers, queue delay, and deadline misses per pipeline.
2. Remove redundant conversions and copies; keep buffers in the producer's native representation until a consumer boundary requires conversion.
3. Reuse compatible encoder outputs and temporal state instead of rereading full frames for each task.
4. Quantize weights and activations where accuracy and hardware support allow; fuse operators and reuse fixed workspaces.
5. Schedule high-bandwidth stages by freshness/deadline and smooth bursts that would contend with the release path.
6. Reduce sensor or model rate adaptively only within declared quality and safety limits.

Zero-copy is a means, not a blanket rule: ownership transfer, synchronization, and incompatible device memory may cost more than one bounded copy. Every optimization should report p50/p95/p99 time-to-action, worst observed deadline slack, bandwidth, energy per useful update, accuracy, and thermal steady state.

## Release gate and degradation

**Maturity: the single release module exists in `runtime/dispatch`; freshness and watchdog hardening remain planned.** Dispatcher is deterministic and contains no learned model. It currently validates the schema, target, declared primitive, numeric limits, base-velocity lease, stop/estop latch, and backend result immediately around release. Future work adds explicit command freshness, broader device-health transitions, deadlines, and watchdog inputs to this same action gate.

The principal invariant is asymmetric authority:

```text
release = all(required_conditions)
stop    = any(stop_condition)
```

Adding a new condition can prevent release but cannot create a bypass. A gateway disconnect, stale skill lease, missing reflex update, incompatible model, backend replacement, or missed deadline maps to an explicit degraded state and eventually hold/stop. Recovery requires a new admitted state; stale commands are never replayed automatically.

## Qualification ladder

Algorithms and model-backed skills advance through the same evidence ladder:

1. offline deterministic fixtures;
2. recorded replay with provenance and timing;
3. demo backend and fault injection;
4. shadow mode on target hardware;
5. bounded-authority hardware-in-the-loop operation;
6. production admission for a named platform and envelope.

Passing one platform does not qualify another. Compiler, model artifact, accelerator runtime, memory topology, backend version, and configuration are part of the qualification identity.
