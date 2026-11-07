# Generic process contribution

[中文](../../../../docs_cn/isos/interfaces/packages/process_cn.md) · [Resources](resources.md) · [Owner handoff](../cores/handoff.md)

A `node_contributions.processes[]` entry describes a package process without granting launch authority. IsOS resolves the exact verified pin, matches the registered node type and placement, checks declared resource IDs and digests, and selects the owning core. The owner verifies its lifecycle adapter. IsOS supervises only IsOS-owned direct children; another core supervises its own children.

| Field | Type / unit | Purpose and validation | Absence/default |
| --- | --- | --- | --- |
| `node_type` | string / registered `@v1` identity | Links this process to one registered type. | Required. |
| `entrypoint` | safe relative string / payload path | Verified executable regular file; no fixed filename or package-ID rule. | Required for direct-child process. |
| `lifecycle_adapter` | string / owner capability | Must be declared and accepted by selected owner; unknown adapter fails. | Required. |
| `resources` | array / setting references | Each setting names a registered resource family and descriptor; selected ID is resolved against verified release. | Empty if none. |
| `host_inputs` | array / declared host requirements | Named inputs verified by the owner; values never enter release or saved graph. | Empty if none. |

The bound `isos.package-process@v1` handoff carries the verified executable and resources, owning core, lifecycle adapter, run ID, and saved revision. Dependency order and readiness/stop conditions remain in the IsOS runtime plan and owner policy; they are not fields in this handoff. Owner rejection, failed readiness, or uncertain stop never becomes a successful replacement. The [handoff contract](../cores/handoff.md) defines wire fields.

## Generic skill selection

A skill library is selected only by exact package pin and a declared regular file with SHA-256. IsOS verifies the release and selected bytes; the owning runtime validates ABI and callable semantics. Recursive file discovery never grants execution authority. Selection changes require revision-checked Apply and revalidation before Run and Restart. A missing or changed library fails before active replacement.
