# Runtime API v1

[中文](../../../../docs_cn/isos/develop/http/runtime_cn.md) · [Stop proof](../../interfaces/cores/dependent-stop.md) · [Log cursors](../owner/log-cursors.md)

**Implemented on the IsOS side.** IsOS owns deployment command admission, status, and bounded log reads. All routes require loopback Host/Origin admission. The common HTTP envelope is `{schema_version:"0.1.0",ok,data,error}`; errors carry stable `code`, safe `message`, and `correlation_id`. A command returns only after the manager responds, and a stop result is never called confirmed without owner proof.

## GET /api/isos/v1/runtime/process-status

Returns the manager's status including saved and active revisions, process state, run ID, and retained terminal outcome. Example response shape: `{"schema_version":"0.1.0","ok":true,"data":{"state":"stopped","run_id":null,"processes":[],"packages":[]},"error":null}`. `packages` is an array of exact `{id,version,path}` graph pins; an empty array means no selected pins. Additional health fields follow the [status contract](../../interfaces/cores/dependent-stop.md).

## POST /api/isos/v1/runtime/control

Request `{"schema_version":"0.1.0","operation":"stop","revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}`. `operation` is `run`, `start`, `stop`, or `restart`; `revision` is an optional exact saved-byte SHA-256 fence. Successful `data` is the manager's operation result. A stale revision returns 409; owner failure or uncertain stop returns a structured fault and never claims success. This route cannot bypass EOS actuator admission.

## GET /api/isos/v1/runtime/logs

Reads bounded sanitized JSONL with query `source`, `cursor`, `limit`, `level`, and optional current-run `view`. Example request: `GET /api/isos/v1/runtime/logs?source=all&limit=100&level=info`; `data` contains `lines`, `next_cursor`, and `rotated`. `limit` is at most 200 records and 64 KiB per call; a rotated cursor is explicit. See [log cursors](../owner/log-cursors.md) for current-run anchors and gap diagnostics.

`schema_version` is string/— and must equal `0.1.0` on mutations; `operation` is string/— and required; `revision` is optional string/SHA-256, absent only when caller accepts current saved revision; `cursor` is opaque string/— and never parsed by clients; `level` is one of five shared severities. These fields separate a saved edit from the active run and keep log reads bounded. Stop, readiness, and errors are observable through the manager result and the deployment event stream.

## Failure example and field contract

For `POST /api/isos/v1/runtime/control`, an illustrative structured failure is:

```json
{"schema_version":"0.1.0","ok":false,"data":null,"error":{"code":"invalid_request","message":"operation must be run, start, stop, or restart","details":null,"correlation_id":"req-42"}}
```

`operation` is required `run`, `start`, `stop`, or `restart`; `run` and `start` select the same manager start operation. `revision` is an optional exact saved-byte fence. `cursor` is opaque, and `limit` is a bounded record count. A successful control response contains the manager operation data inside the shared envelope. The example correlation ID is generated per failure; see [HTTP](envelope.md) for common envelope fields and status mapping.
