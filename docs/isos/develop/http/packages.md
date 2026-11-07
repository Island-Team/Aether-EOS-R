# Packages HTTP API

[中文](../../../../docs_cn/isos/develop/http/packages_cn.md) · [Envelope](envelope.md) · [Generic package contract](../../interfaces/packages/README.md)

All routes require loopback Host/Origin admission. Requests and responses use `schema_version:"0.1.0"`. Install/link/preview never change the saved graph or active run. The package ID is data, never a parser switch.

| Method and route | Input | Success | Failure |
| --- | --- | --- | --- |
| `GET /api/isos/v1/packages` | No body | `data.installed` rows with exact pin and verification status. | 403 Host/Origin; 503 store error. |
| `POST /api/isos/v1/packages/install` | Exact `id`, `version`; optional approved catalog selection. | Verified receipt, path, updated inventory. | 400 malformed; 409 duplicate pin; 422 unsupported contract or unsafe asset. |
| `POST /api/isos/v1/packages/link` | Exact `id`, `version`, absolute `path`, optional absolute `manifest`. | Verified link receipt and inventory. | 400 malformed; 409 duplicate pin; 422 unsupported or changed payload. |
| `POST /api/isos/v1/packages/preview-link` | Absolute bundle `path`, optional `manifest`. | `id`, `version`, `ready`, bounded `issues`; no publication. | 400 malformed; 422 unsafe bundle. |

```json
{"schema_version":"0.1.0","id":"vendor/sample-a","version":"1.2.3","path":"/absolute/bundle","manifest":"/absolute/bundle/release.json"}
```

A successful link returns `{"schema_version":"0.1.0","ok":true,"data":{"verified":true,"source_kind":"local_link"},"error":null}` plus the documented receipt and inventory fields. A contribution missing required registered declarations fails admission. Package removal refuses saved or active exact-pin references. Fields of the release, receipt, resources, and diagnostics are authoritative under [package interfaces](../../interfaces/packages/README.md).
