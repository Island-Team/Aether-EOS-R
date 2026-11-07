# Catalog HTTP API

[中文](../../../../docs_cn/isos/develop/http/catalog_cn.md) · [Envelope](envelope.md) · [Graph](../../interfaces/nodes/README.md)

The catalog routes expose registered node types, selected types, node options, and node-set previews. They derive results from verified generic `node_contributions` and the selected owner catalog; no package ID changes parsing or priority. All routes require loopback Host/Origin admission.

| Route | Result | State effect |
| --- | --- | --- |
| `GET /api/isos/v1/catalog/node-types` | Registered types and availability. | None. |
| `GET /api/isos/v1/catalog/node-types/selected` | Types referenced by applied graph. | None. |
| `POST /api/isos/v1/catalog/node-options` | Choices for declared settings and stable resource IDs. | None. |
| `GET /api/isos/v1/catalog/node-sets` | Verified declared node sets. | None. |
| `POST /api/isos/v1/catalog/node-sets/preview` | Candidate graph composition; never publication. | None. |

Requests use `schema_version:"0.1.0"` where a body is required. Malformed schema is 400; unknown exact pin, type, resource, or owner returns a bounded admission diagnostic. A stale draft revision is 409. Details of field identity and validation are in [graph](../../interfaces/nodes/instance.md), [options](../../interfaces/nodes/selectors.md), and [resources](../../interfaces/packages/resources.md).
