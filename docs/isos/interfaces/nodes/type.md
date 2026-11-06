# Registered node type

This contract is for package publishers and core authors defining a reusable node. IsOS currently validates one registered declaration before an instance can be admitted; the declaration owns placement, settings, ports, lifecycle, and capabilities. An instance cannot widen those declarations. See [instances](instance.md), [ports](ports.md), and [selectors](selectors.md).

| Field | Type / unit | Meaning and validation | Absence/default |
| --- | --- | --- | --- |
| `id` | nonblank string / versioned type identity | Identifies this declaration in the merged owner and package catalog; duplicate IDs are ambiguous. | Required. |
| `core`, `layer`, `group` | nonblank strings / graph path components | Fix the only valid placement for instances. | Required; no inferred owner. |
| `kind` | nonblank string / node class | Describes the node's role for discovery. | Required. |
| `description` | nonblank string / display text | Explains the node to an editor user; it does not grant capability. | Required. |
| `lifecycle` | nonblank string / owner lifecycle ID | Tells the selected core how the node participates in launch and stop. | Required. |
| `capabilities` | array of nonblank strings / capability IDs | Declares features the owner must support; `bridge` is required when `bridge` is present. | Required; empty array allowed. |
| `settings` | array of setting declarations | Defines all accepted instance values, types, defaults, and selectors. Duplicate names fail. | Required; empty array allowed. |
| `inputs`, `outputs` | arrays of port declarations | Declare every connectable input and output. Names are unique within each direction. | Required; empty arrays allowed. |
| `bridge` | object / adapter declaration | Names an allowed transport adapter and the ports it exposes; every named port must be declared. | Optional; absent means no bridge authority. |

The type contract is generic: a UI producer, an HTTP gateway, a primitive binding, and a hardware node use the same fields. The owner defines execution semantics. IsOS checks structure and graph placement; it does not interpret hardware payloads. [Data types](data-types.md) define what ports carry, and [edges](edges.md) define how ports connect.

[中文](../../../../docs_cn/isos/interfaces/nodes/type_cn.md)
