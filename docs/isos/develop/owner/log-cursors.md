# Current-run log cursors

[中文](../../../../docs_cn/isos/develop/owner/log-cursors_cn.md) · [Debugging](../../debug/troubleshooting.md)

A log cursor is opaque to clients and bound to one source and run. The server checks its version, source, run ID, offset, and bound before reading. A stale or forged cursor returns a structured error and never aliases records from a new run. Combined logs include only validated owner events; raw child streams remain diagnostic.

| Field | Type / unit | Purpose and validation | Absence/default |
| --- | --- | --- | --- |
| `cursor` | opaque string / continuation | Encodes source, run, and offset with server integrity check. | Optional on first page. |
| `limit` | positive integer / records | Bounds one response; server enforces maximum. | Server default. |
| `source` | registered string / stream | One owner source or combined stream. | Required. |
| `run_id` | bounded string / run | Must match current selected run. | Required for continuation. |
