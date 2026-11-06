# Presentation HTTP route

This contract is for local clients requesting a representation of one active graph output. The route is implemented but no owner currently registers verified public descriptors, so a valid current request returns 503. The [data type](../../interfaces/ui/selection.md) and [descriptor](../../interfaces/ui/presentation.md) contracts define the internal selection boundary.

## Authentication and request

`POST /api/isos/v1/presentations/select` requires loopback Host/Origin admission. It accepts a JSON body with `schema_version: "0.1.0"`; unknown fields and malformed JSON fail. The shared [envelope](envelope.md) defines errors and correlation IDs.

| Field | Type / unit | Purpose and validation | Absence/default |
| --- | --- | --- | --- |
| `schema_version` | string / version | Must equal `0.1.0`; numeric values fail with `unsupported_schema_version`. | Required. |
| `node_path` | string / graph path | Canonical four-segment output owner path; bounded and validated. | Required. |
| `port` | string / output port ID | Identifies one output; bounded and validated. | Required. |
| `accept` | nonempty array / renderer methods | Client-supported `json`, `text`, `chart`, `image`, `canvas`, `overlay`, or `audio`; order is preference. | Required. |
| `run_id` | string / run identity | Must match the active run. | Required. |
| `graph_revision` | string / SHA-256 | Must match the active revision. | Required. |

```json
{"schema_version":"0.1.0","node_path":"eos/interface/gate/front","port":"preview","accept":["image","canvas"],"run_id":"r7-cde","graph_revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}
```

## Current results

Malformed selection returns 400 `invalid_request`; wrong schema returns 400 `unsupported_schema_version`. A stale run or revision returns 409 `revision_conflict`. A current request with no registered descriptor returns 503 `owner_unavailable`:

```json
{"schema_version":"0.1.0","ok":false,"data":null,"error":{"code":"owner_unavailable","message":"presentation owner has not registered a public delivery descriptor","details":null,"correlation_id":"req-43"}}
```

The route does not return a success envelope or endpoint ID today. Stop, Restart, and Apply invalidate any earlier run/revision selection. The producer retains bytes and endpoint authorization.

[中文](../../../../docs_cn/isos/develop/http/presentations_cn.md)
