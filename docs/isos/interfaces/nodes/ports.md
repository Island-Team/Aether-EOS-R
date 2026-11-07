# Node ports

This page is for core and package authors declaring how nodes exchange data. IsOS currently checks each declared port during type registration and each referenced port during graph admission. The owner remains responsible for delivery and payload authorization. [Data types](data-types.md) define payload shape; [edges](edges.md) define compatibility.

| Field | Type / unit | Meaning and validation | Absence/default |
| --- | --- | --- | --- |
| `name` | nonblank string / local port ID | Identifies one input or output; unique within its direction. | Required. |
| `data_type` | nonblank string / registered data type ID | Binds the port to a declared payload schema. | Required. |
| `required` | boolean / connection rule | Declares whether the owner needs this input or output for an instance. | Required. |
| `transport` | enum / communication method | `in_process`, `unix_socket`, `websocket`, `ros2`, or `http`; edges must agree or pass a declared bridge. | `in_process`. |
| `delivery_mode` | enum / delivery semantics | `message`, `event`, or `stream`; source and destination must agree. | `message`. |
| `cardinality` | enum / connection count | `one` or `many`; admission limits incoming connections. | `one`. |
| `expected_rate_hz` | finite positive number / events per second | Publisher's expected rate for planning and display, not a scheduler grant. | Optional; no expectation. |
| `max_rate_hz` | finite positive number / events per second | Declared rate ceiling for the owner to enforce; registration checks its bounds. | Optional; no declared ceiling. |
| `description` | nonblank string / display text | Explains the value and unit to graph authors. | Required. |

An `http` anchor identifies an HTTP transfer, while `in_process` identifies a local typed handoff. These are graph declarations, not endpoint URLs. Runtime endpoint identity and grants are resolved after admission. A bridge type declares its `adapter` and named `inputs`/`outputs`; its capability must include `bridge`.

[中文](../../../../docs_cn/isos/interfaces/nodes/ports_cn.md)
