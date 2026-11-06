# Graph API v1

[中文](../../../../docs_cn/isos/develop/http/graph_cn.md) · [Graph model](../../interfaces/nodes/instance.md)

**Implemented on the IsOS side.** IsOS owns the saved graph and exact-byte revision. All routes require loopback Host/Origin admission. JSON mutations require `schema_version:"0.1.0"`; Apply is revision checked. The common response is `{"schema_version":"0.1.0","ok":true,"data":{},"error":null}`. Errors use `ok:false`, `data:null`, and an object with stable `code`, safe `message`, and `correlation_id`; admission diagnostics may appear in `details`. Input faults use 400, missing saved data 404, stale revision 409, and admission faults 422.

## GET /api/isos/v1/graph

Reads `{revision,graph,read_only,diagnostics}` without changing saved bytes. A missing graph yields an editable draft and empty revision. Example response: `{"schema_version":"0.1.0","ok":true,"data":{"revision":"","graph":{"version":"0.1.0","cores":{}},"read_only":false,"diagnostics":[]},"error":null}`.

## PUT /api/isos/v1/graph

Applies a complete graph. `revision` is the exact saved-byte SHA-256, or `""` if absent; `graph` is a JSON object. Example request: `{"schema_version":"0.1.0","revision":"","graph":{"version":"0.1.0","cores":{}}}`. A stale revision returns `{"schema_version":"0.1.0","ok":false,"data":null,"error":{"code":"revision_conflict","message":"graph revision changed","details":null,"correlation_id":"req-42"}}` and leaves the saved graph and active child unchanged.

## POST /api/isos/v1/graph/validate

Checks a draft without writing: `{"schema_version":"0.1.0","graph":{"version":"0.1.0","cores":{}}}`. A valid result contains `{"valid":true,"diagnostics":[]}` inside `data`; diagnostics use stable codes and JSON Pointer paths.

## POST /api/isos/v1/graph/migration/preview

Reads the saved graph without writing. Request `{"schema_version":"0.1.0","revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}`; response `data` contains `revision`, `proposed_revision`, `graph`, `changes`, `can_apply`, and `diagnostics`.

## POST /api/isos/v1/graph/migration/apply

Commits exactly a previously previewed graph. Request `{"schema_version":"0.1.0","revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","proposed_revision":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}`; response `data` contains `revision`, `graph`, and `active_revision`. Either changed digest returns 409 before publication.

`schema_version` is string/— and must equal `0.1.0`; `revision` and `proposed_revision` are strings/SHA-256 and must be 64 lowercase hex characters except the empty absent-graph revision; `graph` is an object/— and follows the registered node profile; `changes` is an array/— of bounded JSON Pointer substitutions. These fields exist to keep a draft, saved graph, and active run distinct; no default graph revision is inferred on mutation.

## Failure example and field contract

For `PUT /api/isos/v1/graph`, an illustrative structured failure is:

```json
{"schema_version":"0.1.0","ok":false,"data":null,"error":{"code":"revision_conflict","message":"request conflicts with current state","details":null,"correlation_id":"req-42"}}
```

`revision` is the exact SHA-256 of saved bytes, or empty only when no graph exists; `proposed_revision` identifies precisely the previewed migration output. A revision mismatch does not publish a graph or replace a child. `diagnostics` entries identify a stable code and JSON Pointer path. The example correlation ID is generated per failure; see [HTTP](envelope.md) for common envelope fields and status mapping.
