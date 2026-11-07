# Presentation descriptor boundary

This page explains the typed descriptor used by the pure selector. It is for owner and client implementers. A descriptor identifies a registered output and one representation; it carries no payload bytes or endpoint authorization. The live HTTP route currently has no verified descriptor registration and returns 503 after input and run checks.

| Field | Type / unit | Purpose and validation | Absence/default |
| --- | --- | --- | --- |
| `node_path`, `port` | strings / graph path and port ID | Identify one output; both must be valid, bounded tokens. | Required. |
| `data_type` | `DataType` / semantic type | Carries codec, media type, schema, and payload bound for selection. | Required. |
| `representation_id` | string / identity | Distinguishes producer representations for one output. | Required. |
| `method` | enum / renderer | One of `json`, `text`, `chart`, `image`, `canvas`, `overlay`, `audio`. | Required. |
| `delivery` | enum / transport | `http` or `web_socket`; does not grant access. | Required. |
| `endpoint_id` | string / opaque key | Identifies a producer route; it is not a URL or private grant. | Required. |
| `run_id`, `graph_revision` | strings / run ID and SHA-256 | Fence the descriptor to one active run and graph revision. | Required. |

The pure [selector](selection.md) checks field shape, codec/media/method compatibility, payload bound, and requested run/revision. Registered port/type binding is an upstream owner-admission responsibility; the current HTTP route cannot perform live descriptor selection. The [HTTP contract](../../develop/http/presentations.md) defines its current request and errors.

[中文](../../../../docs_cn/isos/interfaces/ui/presentation_cn.md)
