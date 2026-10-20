# Skill observability

[总览](../skills_cn.md) · [Manager](manager_cn.md) · [Data 与 time](data_cn.md) · [English](../../../../../docs/eos/design/runtime/skills/observability.md)

Skill observability 包含三个不同 store：live process table、service stream 暴露的 bounded per-execution journal，以及可选 persistent local operator diagnostic journal。它们都不是 workflow database。

Lifecycle、child、IPC、capability、checkpoint 与 terminal transition 携带 execution/parent/operation/request/correlation/causation ID、runtime/skill identity、reason/severity、canonical timestamp 与相应 latency。cursor retention loss 用明确 reset/gap 表示，不能把 retained suffix 伪装成完整历史。

Persistent diagnostic 在 manager record lock 外异步写入 `var/log/eos/runs/<launch-id>/eosd/skill-diagnostics.jsonl`。record framed 且 checksummed；incomplete crash tail 会 repair，file 按 configured bound rotate，persistence health 报告 degraded/failed write。boot ID 区分 restart。

## Operator query

使用 installed command：

```text
eos-skill-journal FILE [--execution ID] [--skill NAME]
  [--operation ID] [--correlation ID] [--boot ID]
  [--severity LEVEL] [--from RFC3339] [--to RFC3339]
```

它逐行输出 matching JSON record。interactive REPL 的 `journalctl EXECUTION [SEQUENCE]` follow public execution journal，并非 persistent-file query command。

## Privacy

正常 log 与 diagnostic persistence 不记录 argument、result、SIGNAL/IPC body、frame、speech、credential、resource value、primitive acknowledgement body 或 raw protobuf；只保留 bounded identifier、schema、size、digest、state、reason 与 timing。payload capture 不是普通 debug shortcut；如果 deployment policy 启用，必须 encrypted、permission-restricted、audited 并自动 expiry。
