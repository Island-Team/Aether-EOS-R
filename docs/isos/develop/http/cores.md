# Cores API v1

[中文](../../../../docs_cn/isos/develop/http/cores_cn.md) · [Owner boundaries](../owner/README.md)

**Implemented on the IsOS side.** IsOS reports configured EOS/EGIA readiness and calls their bounded setup adapters. All routes require loopback Host/Origin admission. The shared HTTP envelope is `{schema_version:"0.1.0",ok,data,error}`; an incompatible or unavailable owner returns a structured 503, not an empty success.

## GET /api/isos/v1/cores/readiness

Returns `data.cores`, graph and IsOS/UI configuration readiness. It describes selected installed paths and compatibility, not proof that every endpoint is listening.

## GET /api/isos/v1/cores/setup-status

Returns `data.ready`, `issues`, `graph_revision`, backend choices, and process summary. Example response: `{"schema_version":"0.1.0","ok":true,"data":{"ready":false,"issues":["selected owner setup is unavailable"],"graph_revision":null,"backends":[],"process":null},"error":null}`.

## GET /api/isos/v1/cores/setup/{role}/catalog

`role` is exactly `eos` or `egia`. IsOS invokes the selected setup adapter's numeric-schema-2 catalog operation without starting its runtime and returns the typed owner reply in `data`.

## POST /api/isos/v1/cores/setup/{role}/validate

Request `{"schema_version":"0.1.0","graph":{"version":"0.1.0","cores":{}},"revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}`. IsOS passes the draft and revision to the selected owner; no configuration is published. The owner reply contains numeric `schema_version:2`, `ok`, `diagnostics`, `files`, and optional `identity` or `catalog`.

## POST /api/isos/v1/cores/setup/{role}/render

Uses the same request shape as validate and previews bounded relative generated files without publishing them. A missing graph is 400; unknown role or operation is 404; timeout, malformed reply, or setup identity without `setup_schema_version:2` is 503.

`schema_version` is string/— and must equal `0.1.0`; `role` is a closed string/— to prevent selecting arbitrary executables; `graph` is the registered profile object/—; `revision` is an optional 64-character lowercase SHA-256 string when the caller already has a saved-byte fence. The external setup wire uses schema 2 and bounded stdin/stdout; a product version does not imply setup compatibility.

## Failure example and field contract

For `POST /api/isos/v1/cores/setup/eos/validate`, an illustrative structured failure is:

```json
{"schema_version":"0.1.0","ok":false,"data":null,"error":{"code":"owner_unavailable","message":"required owner is unavailable","details":null,"correlation_id":"req-42"}}
```

`role` is the path selector `eos` or `egia`; unknown roles are not executable selectors. `graph` is the required draft object for validate/render, `revision` is the optional exact saved-byte SHA-256 fence, and `schema_version` must equal `0.1.0`. The setup adapter uses the separate contract in [setup](../../interfaces/cores/setup.md). The example correlation ID is generated per failure; see [HTTP](envelope.md) for common envelope fields and status mapping.
