# Version API

[中文](../../../../docs_cn/isos/develop/http/version_cn.md)

**Implemented on the IsOS side.** IsOS owns the local compatibility identity. The route requires loopback Host/Origin admission. It is read-only and does not probe or launch another core. Errors use the shared schema-0.1.0 structured HTTP envelope.

## GET /api/isos/v1/system/version

Returns the independent API schema, IsOS package version, and bundled local version-support map. Example response: `{"schema_version":"0.1.0","ok":true,"data":{"api_schema_version":"0.1.0","isos_version":"0.2.1","version_support":{"schema":"isos.version-support@v1","product_version":"0.2.1","target_graph_format":"0.2.0","isos_http_schema":"0.1.0","package_contract":"isos.package@v1","package_schema":"0.1.0","external_packages":[{"id":"island/mujoco-sim","version":"0.1.1","manifest_sha256":"c421039753c24ffc74658d6b1dfe5c9de6a1ed77b64df8fbaee77063da837096","asset_sha256":"bb2862b01c3ccab8b68570e766a0ad26c8c5783b3c87cf99225eb0edb7a260f4"}]}},"error":null}`. The version map is compiled from `config/version-support.toml` and does not contact GitHub. Clients reject an unsupported `api_schema_version`; a product version is not a wire compatibility test. A rejected Host or Origin returns a safe structured 403 error.

| Field | Type / unit | Purpose and validation | Absence/default |
| --- | --- | --- | --- |
| `api_schema_version` | string / contract version | Exact current IsOS HTTP envelope schema, `0.1.0`. | Required. |
| `isos_version` | string / product SemVer | Installed IsOS source package version; independent of HTTP. | Required. |
| `version_support.schema` | string / identity | Bundled map contract, `isos.version-support@v1`. | Required. |
| `version_support.product_version` | string / product SemVer | Must match the built IsOS package version. | Required. |
| `version_support.target_graph_format` | string / graph format | Target graph format for this build; `0.2.0`. | Required. |
| `version_support.isos_http_schema` | string / contract version | Must match this response's HTTP schema. | Required. |
| `version_support.package_contract`, `.package_schema` | strings / package contract | Exact manifest identity and schema accepted by the current package reader. | Required. |
| `version_support.external_packages[]` | array / exact pins | Reviewed external package IDs, versions, and 64-hex manifest/asset SHA-256 values. | Empty when none. |

`isos version check --json` includes the same `version_support.map` plus a local graph status: `compatible` for the exact target format, `migration_required` for supported 0.1.0 input, `unsupported` for another format, or `not_configured` when no graph is saved. This is local identity reporting, not an online latest-release check or an automatic graph migration. The [target package manager design](../../design/package-manager-target.md) describes explicit online update checks.

## Failure example and field contract

For `GET /api/isos/v1/system/version`, an illustrative structured failure is:

```json
{"schema_version":"0.1.0","ok":false,"data":null,"error":{"code":"unauthorized","message":"unauthorized","details":null,"correlation_id":"req-42"}}
```

The route has no request body and no query fields. The example correlation ID is generated per failure; see [HTTP](envelope.md) for common envelope fields and status mapping.
