# Skill data and time

[Overview](../skills.md) · [IPC](../../../develop/interfaces/skills/native/ipc.md) · [Observability](observability.md) · [中文](../../../../../docs_cn/eos/design/runtime/skills/data_cn.md)

All newly exchanged skill data uses `EventTime` so event time, receipt time, and ordering are not confused.

| Field | Meaning |
| --- | --- |
| `source_timestamp_ns` | Optional producer event time, interpreted only in `source_clock_id`. |
| `source_clock_id` | `utc`, EOS boot-monotonic, or a named device clock domain. |
| `observed_at` | UTC time when EOS received or created the event. |
| `observed_monotonic_ns` | EOS monotonic time for latency and ordering within one boot. |
| `boot_id` | Identity of the `eosd` process epoch. |
| `sequence` / `event_id` | Order within the documented scope and stable event identity. |

EOS never converts an unknown device clock into invented UTC. Producer data may arrive out of order; manager sequence records admission order. Comparisons across boots use UTC only when it is actually available, never monotonic values.

## Sources

Definitions declare source topic, JSON Schema, and read/write authority. Publications retain producer time and receive canonical observation metadata. `latest`, `exact`, `before`, `after`, and `nearest` operate on producer time; sequence breaks equal-time ties. `wait` returns the oldest retained sample after a sequence and wakes on cancellation or shutdown. Timelines are bounded and process-local.

## Public contract

`share/contracts/eos/skills.proto` adds event time, boot and parent identity, source and IPC declarations, and repeated process activity additively. Old clients continue to read existing fields. New clients must tolerate absent additive fields from older servers and unknown future fields.
