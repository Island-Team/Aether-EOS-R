# Bound package process handoff

[中文](../../../../docs_cn/isos/interfaces/cores/handoff_cn.md) · [Process declarations](../packages/process.md) · [Core index](README.md)

The `isos.package-process@v1` handoff is one bounded, private, run-bound JSON message. It is written only after exact-pin release, payload, registered type, resource, and owner-capability verification. The selected owner must recheck its adapter and package-specific semantics before readiness. An unknown owner or adapter rejects without child spawn.

| Field | Type / unit | Purpose and validation | Absence/default |
| --- | --- | --- | --- |
| `contract` | exact string / identity | `isos.package-process@v1`. | Required. |
| `schema_version` | exact string / version | `0.1.0`. | Required. |
| `owner_core` | string / core ID | Registered owner for the selected node type. | Required. |
| `run_id`, `graph_revision` | bounded string; lowercase 64-hex / run and SHA-256 | Fence this handoff to one admitted run and saved graph. | Required. |
| `node_path`, `node_type` | strings / canonical path and registered type | Identify the selected process. | Required. |
| `package` | `{id,version}` / exact pin | Selects one verified release, never latest. | Required. |
| `entrypoint`, `resources` | verified paths and digests / files | Contained executable and selected resource files; no package-specific filename. | Required; resources may be empty. |
| `lifecycle_adapter` | registered string / owner capability | Owner must accept it before spawn. | Required. |
| `release_sha256`, `payload_sha256` | lowercase 64-hex / bytes | Bind accepted manifest and payload. | Required. |

Dependency order and readiness/stop conditions remain in the runtime plan and owner policy; they are not fields in this handoff. It is not a public endpoint, graph field, or actuator grant.
