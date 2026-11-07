# Generic package resources

[中文](../../../../docs_cn/isos/interfaces/packages/resources_cn.md) · [Release](release.md) · [Process](process.md)

A verified release may declare resources for selectors and process settings. The descriptor contract is `isos.package-resources@v1` with `schema_version:"0.1.0"`; it contains at most 256 resource records and is bounded to 256 KiB. IsOS bounds the descriptor, verifies declared file containment and digest, and exposes stable IDs. It does not parse resource bytes or interpret relationships in owner metadata. The selected core must validate semantics before readiness. A resource file is never a graph setting or executable authority.

| Field | Type / unit | Purpose and validation | Absence/default |
| --- | --- | --- | --- |
| `kind` | bounded string / family | Names the registered resource family; not a hard-coded package class. | Required. |
| `id` | bounded string / stable selector ID | Unique with `kind` in one release; path-free and suitable for graph storage. | Required. |
| `label` | bounded string / display text | Human-readable choice name; cannot affect admission. | Required. |
| `description` | bounded string / display text | Selection guidance; cannot affect admission. | Required. |
| `path` | safe relative string or null / payload path | Optional regular file within the verified payload. | Null for logical resources. |
| `sha256` | lowercase 64-hex string or null / file bytes | Exact digest, rechecked at admission; present iff `path` is present. | Null for logical resources. |
| `metadata` | bounded JSON value / owner-defined | At most 16 KiB serialized; opaque to IsOS, with relationships validated by owner. | Optional, null. |

Unknown IDs, duplicate `(kind,id)`, changed bytes, escaping paths, and symlinks fail with bounded package diagnostics. The canonical graph pointer is retained in errors. A stale revision fails before mutation. The same checks run before Apply, Run, and Restart.

```json
{"kind":"configuration","id":"balanced","label":"Balanced","description":"General profile","path":"profiles/balanced.json","sha256":"0000000000000000000000000000000000000000000000000000000000000000"}
```

The hash is format-only. A publisher must use the real file digest.
