# Runtime monitoring and emergency latch

[中文](../../../docs_cn/ei/core/monitor.md) · [Core overview](modules.md) · [Safety](safety.md)

`egia.core.monitor` turns transport health, execution inactivity and measured base motion into attributed fault evidence. It also owns the 60-second workload rate window and the fail-closed fleet estop latch. It does not execute ordinary work or decide whether a mission is semantically correct.

## Heartbeat loop

`monitor_entry` normalizes one client or a backend mapping, creates independent failure counters and escalation episodes, sets readiness, and ticks every `heartbeat_interval_s`. Each tick probes every backend; one slow probe currently delays later probes because checks are sequential.

On success after any failure, monitor forces rediscovery. It then refreshes the typed status aggregate; either failure counts as a failed tick. Successful rediscovery updates the status tracker and, while estop is engaged, attempts restart reconciliation. A fully successful tick records tracker success, emits one `heartbeat recovered` observation if the backend had escalated, and resets its counter. A failed health check or refresh increments the counter. The first failure is logged; reaching `failure_threshold` emits exactly one fault per episode. Continued failure is coalesced. `availability_changed` receives whether all counters are zero after each fleet pass.

## Mission-stall process

After heartbeats, `_check_stall` reads the active mission, newest `result` observation and mission start time. No active mission resets the episode. A mission is stalled only when both the last-result age and start age meet `stall_timeout_s`; this protects a slow first inference. Missing result or start timestamps are treated as older than the threshold. The first stalled tick emits a `step_stall` fault, annotates the plan and requests a decision interrupt. Further ticks coalesce until recent activity or no active mission re-arms it.

## Bounds and rate state

`PhysicalBounds.check_base` computes planar speed with `hypot(x, y)` before checking absolute yaw rate. Defaults are 2 m/s and 3.14159 rad/s; speed wins if both violate. `BoundWatchdog` ignores non-base snapshots. Every violating frame emits a fault with measured value and limit. Five consecutive violations by default engage estop once; one healthy frame resets the streak and re-arms it. The streak is global to this watchdog, not keyed by backend or base device. Non-finite measurements are not explicitly rejected here and therefore rely on typed contract validation upstream.

`RateWindow.admit` uses monotonic time, discards timestamps at least 60 seconds old, and records one slot only for an admitted attempt. A later transport failure still consumes that slot. Rejection does not append. The deque is in-memory and resets with the process.

## Emergency lifecycle

Engagement is idempotent and fsyncs the local barrier with every affected alias and EOS robot identity before transport. For each backend, the latch calls process-wide `safety.estop`; errors or EOS refusal are logged per backend and never abort the fleet loop. Unknown EOS identity is stored as empty and cannot pass authorized reset. `motion_stop()` separately discovers every backend and best-effort calls `motion.stop` for each motion device, continuing after every failure.

There is no release RPC. `reconcile_restarts()` always returns false; heartbeat, reconnect, EOS restart and EGIA restart cannot clear the durable latch. The stopped-process `egia safety reset` CLI writes one-shot authorization under an exclusive lock. The next launch performs two fresh complete selected-revision checks with exact alias and robot identity coverage, false EOS estop, safe live status and no unresolved operation before durable unlatch. Cancellation never invents recovery.
