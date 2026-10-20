# Skill observability

[Overview](../skills.md) · [Manager](manager.md) · [Data and time](data.md) · [中文](../../../../../docs_cn/eos/design/runtime/skills/observability_cn.md)

Skill observability has three distinct stores: the live process table, the bounded per-execution journal exposed by service streams, and the optional persistent diagnostic journal for local operators. None is a workflow database.

Lifecycle, child, IPC, capability, checkpoint, and terminal transitions carry execution/parent/operation/request/correlation/causation IDs, runtime and skill identity, reason and severity, canonical timestamps, and relevant latency. Cursor retention loss is represented as an explicit reset/gap rather than silently pretending the retained suffix is complete.

Persistent diagnostics are written asynchronously outside manager record locks to `var/log/eos/runs/<launch-id>/eosd/skill-diagnostics.jsonl`. Records are framed and checksummed; an incomplete crash tail is repaired, files rotate at configured bounds, and persistence health reports degraded or failed writes. Boot IDs separate restarts.

## Operator query

Use the installed command:

```text
eos-skill-journal FILE [--execution ID] [--skill NAME]
  [--operation ID] [--correlation ID] [--boot ID]
  [--severity LEVEL] [--from RFC3339] [--to RFC3339]
```

It emits matching JSON records, one per line. The interactive REPL `journalctl EXECUTION [SEQUENCE]` follows the public execution journal; it is not the persistent-file query command.

## Privacy

Normal logs and diagnostic persistence omit arguments, results, SIGNAL and IPC bodies, frames, speech, credentials, resource values, primitive acknowledgement bodies, and raw protobuf. They retain bounded identifiers, schemas, sizes, digests, states, reasons, and timing. Payload capture is not a normal debugging shortcut; if enabled by deployment policy it must be encrypted, permission-restricted, audited, and expire automatically.
