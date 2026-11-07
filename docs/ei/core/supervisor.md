# Supervised task lifecycle

[中文](../../../docs_cn/ei/core/supervisor.md) · [Core overview](modules.md)

`egia.core.supervisor` is a small asyncio task supervisor. It owns task handles, readiness events and health projections; subsystem data remains owned by the subsystem. The current implementation has one shared stop event and a uniform, unbounded restart policy. It does not model dependency order or per-subsystem restart limits.

## Registration and state

A `SubsystemSpec` contains a unique name and an async entry callable receiving `(ready, stop)`. The entry initializes, sets `ready`, then normally runs until `stop`. `_Runtime` stores the task, fresh readiness event, state, first-ready time, cumulative restarts/failures and last error. Duplicate names in the input list collapse in the internal mapping; composition must therefore guarantee name uniqueness.

## Start and readiness

`start()` spawns every registered entry in specification order and then the watchdog; it does not wait between entries. `_spawn()` replaces the readiness event, clears `ready_at`, marks `STARTING`, and names the task `egia-<name>`. `wait_ready(timeout=30)` waits concurrently for all current readiness events. No specs means immediate success. On timeout it logs the exact missing names and returns false, but does not cancel them. When all are ready it timestamps any runtime not already seen by the watchdog, marks `READY`, and returns true.

There is an intentional race boundary: a task can set readiness and exit before the next watchdog pass; readiness may briefly succeed, but `is_healthy()` also requires every task to be alive. Health snapshots preserve registration order, report zero uptime until first ready, clamp negative uptime to zero, and include restart count and the last exception representation.

## Failure and restart process

The watchdog samples every 0.5 seconds. A completed or cancelled subsystem task is marked `DEGRADED`; exceptions become `repr(exc)`, while clean return or cancellation becomes `task exited without error`. Failure count selects backoff `0.5, 1, 2, 5, 10` seconds, capped at 10 for later failures. After the sleep, if global stop is still clear, restart count increments and `_spawn()` supplies a new readiness event and task. Failure counters and last error remain historical.

Restarts are processed sequentially inside the watchdog. A backoff for one task delays detection/restart of other completed tasks. There is no maximum restart count, dependency cascade, jitter, state checkpointing, or separate optional failure class. Domain loops must make their own initialization atomic and recover their own durable state.

## Stop and edge conditions

`stop()` marks every runtime `STOPPING`, sets the shared event, cancels the watchdog, and gathers all subsystem tasks with `return_exceptions=true`. It has no internal graceful timeout and does not cancel entries itself, so every entry must observe `stop`; a non-cooperative entry can block shutdown. Exceptions during this gather are consumed, after which shutdown is logged. A concurrent watchdog backoff observes the stop event before respawning.

Calling `start()` or `stop()` repeatedly is not guarded by an explicit lifecycle state; composition calls each once. The supervisor itself does not enforce the desired provider-before-consumer startup or dependent-before-provider shutdown order. Those safety properties arise from composition, readiness-aware entries, the shared stop protocol, and explicit resource closure in `core.launch`.
