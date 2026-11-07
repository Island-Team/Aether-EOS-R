# Saved node instance

This contract is for graph authors selecting a registered type and wiring its ports. IsOS currently stores instances under `core → layer → group → node` and admits them against the selected type. An instance supplies identity and selections; it does not redefine ports or ownership. See [type](type.md), [settings](selectors.md), and [edges](edges.md).

| Field | Type / unit | Meaning and validation | Absence/default |
| --- | --- | --- | --- |
| `version` | exact string / graph schema | `0.1.0` for the current graph. | Required. |
| `cores` | object / core IDs | Root map of owning cores; keys are stable IDs. | Required. |
| `layers`, `groups`, `nodes` | nested objects / path components | Locate the instance at its type's declared placement. | Required for each used level. |
| `id` | string / local node ID | Must match the `nodes` map key. | Required. |
| `node_type` | string / registered type ID | Selects one type and its settings, ports, lifecycle, and owner. | Required. |
| `package` | `{id,version}` / exact release pin | Selects verified bytes for a package-contributed type; no implicit latest release. | Required for package-contributed types. |
| `settings` | object / declared values | Only type-declared names and value kinds; omitted values use declared defaults. | Optional when all settings permit omission. |
| `inputs` | object / port references | Each key is a declared input; each value names a source node and output. | Empty when no inputs connect. |
| `references` | object / declared node paths | Non-port dependencies resolved at admission. | Empty when no references are declared. |

The graph stores stable resource IDs, not executable or asset paths. IsOS resolves the exact pin and verified resource bytes at admission, then checks current revision before Apply. Run and Restart recheck those bytes. The graph never carries an HTTP endpoint or authorization token.

[中文](../../../../docs_cn/isos/interfaces/nodes/instance_cn.md)
