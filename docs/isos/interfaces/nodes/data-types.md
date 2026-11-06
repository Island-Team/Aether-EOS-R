# Node data types

This contract is for producers, consumers, and IsosUI renderers. A release contribution or core catalog declares a data type once; registered [ports](ports.md) reference its `id`. IsOS currently validates the declaration and admits edges by exact data type identity. A producer owns payload bytes and authorization.

| Field | Type / unit | Meaning and validation | Absence/default |
| --- | --- | --- | --- |
| `id` | nonblank string / type ID | Stable identity used by ports and edges. | Required. |
| `version` | nonblank string / schema version | Version of this payload meaning, independent of package and HTTP versions. | Required. |
| `purpose` | nonblank string / display text | Explains what consumers may use the value for. | Required. |
| `media_type` | nonblank string / MIME type | Describes representation; consumers must also check codec and schema. | Required. |
| `codec` | nonblank string / encoding ID | Identifies exact encoding such as JSON, UTF-8, image, pixel, or audio encoding. | Required. |
| `schema` | JSON object / payload structure | Owner-defined shape and units for structured values or planes. | Required. |
| `max_payload_bytes` | integer 1–100,000,000 / bytes | Caps one encoded payload before presentation or delivery. | Required. |

The same fields can describe records, text, time series, images, pixel planes, depth, detections, and audio. A renderer chooses a method only after checking the registered port, codec, media type, schema, bound, and its own capabilities; [UI selection](../ui/selection.md) describes that choice. Type names alone do not authorize decoding.

[中文](../../../../docs_cn/isos/interfaces/nodes/data-types_cn.md)
