# Owner event contract

[中文](../../../../docs_cn/isos/interfaces/cores/events_cn.md) · [Design](../../design/server.md)

The implemented private event input is a bounded JSON object from the selected owner process. IsOS checks the selected pipe, producer role, core, admitted node path, and active `run_id` before shared logging. It rejects duplicate or unknown keys, malformed or oversized lines, and codes outside the owner policy. This input has **no** `schema_version` or `graph_revision` field; those belong to other boundaries.

| Field | Type / unit | Purpose and validation | Absence/default |
| --- | --- | --- | --- |
| `timestamp` | string / time | Producer timestamp, retained after validation. | Required. |
| `level` | string / severity | Allowlisted severity for `event_code`. | Required. |
| `source`, `core` | strings / identities | Must match the selected process role and core. | Required. |
| `node_path` | string or null / graph path | When present, must be admitted for this owner. | Optional, null. |
| `event_code` | string / event identity | Must be in the compiled owner policy. | Required. |
| `run_id` | string / run | Must match active run. | Required. |
| `correlation_id` | string or null / identity | Correlates related operations. | Optional, null. |
| `message` | bounded string / text | Sanitized diagnostic. | Required. |
| `fields` | JSON object / code-specific | Duplicate keys and disallowed code fields reject. | Optional, empty object. |
