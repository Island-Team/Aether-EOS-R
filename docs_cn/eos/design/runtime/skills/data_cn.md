# Skill data 与 time

[总览](../skills_cn.md) · [IPC](../../../develop/interfaces/skills/native/ipc_cn.md) · [Observability](observability_cn.md) · [English](../../../../../docs/eos/design/runtime/skills/data.md)

所有新 skill exchange 使用 `EventTime`，避免混淆 event time、receive time 与 ordering。

| Field | 含义 |
| --- | --- |
| `source_timestamp_ns` | optional producer event time，仅在 `source_clock_id` 中解释。 |
| `source_clock_id` | `utc`、EOS boot-monotonic 或 named device clock domain。 |
| `observed_at` | EOS 接收或创建 event 时的 UTC。 |
| `observed_monotonic_ns` | 同一 boot 内用于 latency 与 ordering 的 EOS monotonic time。 |
| `boot_id` | `eosd` process epoch identity。 |
| `sequence` / `event_id` | documented scope 内的顺序与 stable event identity。 |

EOS 不把未知 device clock 转换为虚构 UTC。producer data 可乱序到达；manager sequence 记录 admission order。跨 boot 比较只在确实存在 UTC 时使用 UTC，绝不比较 monotonic value。

## Source

definition 声明 source topic、JSON Schema 与 read/write authority。publication 保留 producer time，并获得 canonical observation metadata。`latest`、`exact`、`before`、`after`、`nearest` 按 producer time 工作，以 sequence 打破同 timestamp tie。`wait` 返回指定 sequence 之后最早 retained sample，并在 cancellation/shutdown 时唤醒。timeline 有界且 process-local。

## Public contract

`share/contracts/eos/skills.proto` 以 additive field 公开 event time、boot/parent identity、source/IPC declaration 与 repeated process activity。旧 client 继续读取现有 field；新 client 必须容忍 older server 缺失 additive field 以及未来 unknown field。
