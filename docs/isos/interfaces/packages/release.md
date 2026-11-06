# Package release contract

[中文](../../../../docs_cn/isos/interfaces/packages/release_cn.md) · [Verification](verification.md) · [Resources](resources.md)

`isos.package@v1` is the active IsOS contribution identity. The release manifest uses `"schema_version":"0.1.0"`, and `node_contributions` is its sole active source of registered node types, processes, resources, and node sets. IsOS reads a release only through an exact `{id,version}` pin. A selected type must have a verified generic contribution before Apply, Run, or Restart.

| Field | Type / unit | Purpose and validation | Absence/default |
| --- | --- | --- | --- |
| `contract` | string / identity | Must equal `isos.package@v1`. | Required. |
| `schema_version` | string / version | Must equal `0.1.0`. | Required. |
| `id`, `version` | strings / exact package pin | Canonical package ID and exact release version; never infer latest. | Required. |
| `kind` | string / catalog class | Bounded stable descriptor for discovery; it is metadata and does not select behavior or a parser. | Required. |
| `source_commit` | 40-hex string / Git commit | Publisher provenance; independent of payload digest. | Required. |
| `description`, `license`, `source_url`, `release_url` | strings / display or URL | Catalog metadata only; cannot grant runtime capability. | Optional, absent. |
| `contracts` | string array / identities | Declared external capabilities; owner admission still verifies them. | Optional, empty. |
| `node_contributions` | object / declarations | Registered types, process declarations, data types, and node sets; IsOS validates structural shape and ownership. | Required for Apply/Run. |
| `assets` | array / candidate artifacts | Host-matched, bounded download candidates with URL, byte length, and SHA-256. | Required for archive install. |

A release may contain owner-defined opaque metadata only inside a bounded owner extension. IsOS never interprets that metadata as scene, device, library, or adapter authority. The owning core validates its semantics before declaring readiness. The [process contract](process.md) defines the generic contribution fields.

```json
{"contract":"isos.package@v1","schema_version":"0.1.0","id":"vendor/sample-a","kind":"component","version":"1.2.3","source_commit":"0000000000000000000000000000000000000000","node_contributions":{"node_types":[],"processes":[],"node_sets":[]},"assets":[]}
```

The empty example is structurally illustrative; admission requires the selected registered type and declared owner capabilities. The [CLI publication procedure](../../maintenance/todo.md) is separate from package admission.
