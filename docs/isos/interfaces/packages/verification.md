# Verified package admission

[中文](../../../../docs_cn/isos/interfaces/packages/verification_cn.md) · [Release](release.md) · [Resources](resources.md)

Installation verifies one exact `{id,version}` release and asset, then publishes receipt and payload atomically. A local link freezes the accepted metadata and payload fingerprint. IsOS repeats verification at graph admission, Apply, Run, and Restart; a changed linked source requires explicit relink. No unrelated installed package can block a graph that does not pin it.

Admission is a single sequence: exact pin → release contract and payload receipt → registered node type and placement → declared resource IDs and contained file digests → owner and lifecycle capability → run-bound handoff. The pin collector covers all supported graph pin locations and sorts duplicate diagnostics by canonical graph pointer. There is no package-specific priority or fallback parser.

| Failure | Outcome | State effect |
| --- | --- | --- |
| Missing required generic contributions | `unsupported_package_contract` | No saved graph or active run change. |
| Invalid release, unsafe path, symlink, digest mismatch, or oversized metadata | Bounded package diagnostic with graph pointer and stage | No partial install or replacement. |
| Unknown registered type, placement, owner, or adapter | Typed admission diagnostic | No child spawn or graph publication. |
| Unknown resource ID or changed resource bytes | Typed resource diagnostic | No child spawn or graph publication. |
| Stale saved revision or run fence | Conflict diagnostic | No active replacement. |

Direct install/link errors use an empty graph pointer; graph-time errors use the actual selected node pointer. Diagnostics contain a stable code, stage, exact pin, sanitized message, and canonical pointer; they never expose local paths, raw metadata, or secrets. See [HTTP packages](../../develop/http/packages.md) for the response envelope.

## Reader and feature preflight

IsOS checks the selected executable and its exact verified package contributions before Apply, Run, and Restart. A product version, a different checkout, an executable path alone, or a package type suffix does not prove support. Capability probes are bounded, start no runtime, and must describe the bytes that will launch. A changed executable, interpreter, module tree, package receipt, or generated revision blocks replacement. Missing interpreter does not trigger `uv sync` or another bootstrap. The external setup reply uses schema 2; IsOS-owned HTTP and package contracts use `"0.1.0"` independently.

## Reader identity fields

| Field | JSON type / unit | Purpose, absence and validation |
| --- | --- | --- |
| `supported_graph_versions` | nonempty array of unique strings / graph versions | Exact versions read by the selected binary. Required when the selected reader declares graph-version support. |
| `supported_node_type_versions` | nonempty array of unique strings / type suffixes | Exact suffixes read by the binary; never sufficient to admit a full type ID. |
| `supported_node_types` | array of unique full IDs / types | Built-in IDs implemented by the reader. A contributed type additionally requires a verified exact package pin and owner catalog. |
| `features` | array of unique ASCII strings / capabilities | Actual capabilities of that binary. Missing means empty; required feature then fails. |
| `binary` | string / selected executable identity | Runtime `--capabilities` role, such as `aether-runtime`; setup uses `core`. |
| `product_version` | nonempty string / release version | Display and provenance only, not the compatibility predicate. |
| `setup_schema_version` | integer / external setup schema | Current selected setup identity must equal `2`. |

IsOS checks graph structure, exact pins and verified contributions before consuming the combined owner catalog. It then checks graph/type versions and required features against the selected setup reader, graph-reading runtime, and transport children. A `package-process-v1` contribution requires owner capability; the selected runtime may also require `managed-control-grants`, `managed-config-v1`, or `eos-gateway-client-v2` according to the admitted graph. A missing feature blocks before spawn. An owner declaring a feature does not prove readiness; health is checked separately. Diagnostics sort by canonical graph path, binary role, and feature ID.

## Diagnostic fields

| Field | JSON type / unit | Purpose and validation |
| --- | --- | --- |
| `code` | stable string / failure | One of `reader_identity_invalid`, `reader_graph_unsupported`, `reader_type_unsupported`, `reader_feature_unsupported`, `reader_binary_mismatch`, `reader_package_pin_mismatch`. |
| `path` | JSON Pointer / graph field | Exact selected type/package or `/version`; selected child errors may use `/settings/role`. |
| `message` | bounded string / repair text | Sanitized action; no raw stderr, local paths, or secrets. |
| `stage` | `graph`, `package`, `reader`, `feature` / phase | Identifies the failed admission step. |
| `reader.core`, `.binary`, `.binary_sha256` | strings and optional 64-hex / selected binary | Exact selected owner, executable identity, and byte provenance. |
| `reader.requested` | object / requested claim | Applicable graph version, full type, feature, and exact package pin; inapplicable values are null. |
| `reader.supported` | object / advertised claims | Sorted graph versions, type versions, full IDs, and features; untrusted claims are empty arrays. |

Example bounded diagnostic (the JSON object is shared with Chinese documentation): `{"diagnostics":[{"code":"reader_feature_unsupported","path":"/cores/eos/layers/interface/groups/gate/nodes/preview/node_type","message":"selected owner lacks vision-gate; choose a compatible build","stage":"feature","reader":{"core":"eos","binary":"aether-runtime","binary_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","requested":{"graph_version":"0.1.0","node_type":"eos.interface.camera_gate@v0.1","feature":"vision-gate","package":null},"supported":{"graph_versions":["0.1.0"],"node_type_versions":["0.1"],"node_types":["eos.interface.camera_gate@v0.1"],"features":[]}}}]}`. A verified `compatible_upgrade` can only name an exact checked release; listing it never installs or applies it. The owner migration gate is in [TODO](../../maintenance/todo.md).
