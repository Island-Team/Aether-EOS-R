# UI node events

This contract is for an IsosUI node that emits a user input through a registered output port. IsOS currently exposes `POST /api/isos/v1/nodes/events` for the first key-state producer. The endpoint checks the local UI Host and Origin, checks the active run and saved revision, resolves the declared graph route, and derives movement values from its gateway node's settings. It does not accept velocity or an owner endpoint from the browser.

```json
{"schema_version":"0.1.0","source_node":"isosui/input/keyboard/drive","output":"state","data_type":"key_state","run_id":"run-1","active_revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","payload":{"state":"pressed","activation_id":"press-1","sequence":1}}
```

| Field | Type / unit | Meaning and validation | Absence/default |
| --- | --- | --- | --- |
| `schema_version` | exact string / payload version | `0.1.0`. | Required. |
| `source_node` | canonical string / four-part node path | Registered UI producer in the active graph; at most 200 bytes, four nonempty path segments using ASCII letters, digits, `_`, `-`, or `.`. | Required. |
| `output` | exact string / output port | `state` for this producer; must be declared by its registered type. | Required. |
| `data_type` | exact string / registered type | `key_state` for the declared output. | Required. |
| `run_id` | string / active run identity | Prevents a prior run's events from affecting the current one; 1–128 bytes. | Required. |
| `active_revision` | lowercase 64-hex string / saved graph SHA-256 | Fences the event to the admitted graph. | Required. |
| `payload.state` | enum / key state | `pressed`, `refresh`, or `released`. | Required. |
| `payload.activation_id` | nonblank string / key hold identity | Groups one physical hold and its renewals; 1–64 ASCII letters, digits, `_`, or `-`; cannot be reused within a run (at most 65,536 completed holds per source and run). | Required. |
| `payload.sequence` | positive integer / event order | Strictly increases within an activation; stale or reordered events cannot renew a lease. | Required. |

A successful event returns the IsOS envelope with `data.command_id` (the generated owner command identity) and `data.state` (`active` or `released`). Owner reply fields are not forwarded.

The producer's selected key or chord is a node setting, not an event field. The gateway's target binding path and `x_m_s`, `y_m_s`, `yaw_rad_s` (metres per second and radians per second), and `valid_for_ms` (50–500 milliseconds) are saved settings. IsosUI emits refreshes while a key is held; IsOS bounds renewal to at most 20 Hz and always permits a valid release. Keyup, focus loss, or teardown release the hold; the lease also expires if refresh stops. The owner decides physical actuation and stop. See [the control path](../../design/control-path.md) and [registered selectors](../nodes/selectors.md).

[中文](../../../../docs_cn/isos/interfaces/ui/node-events_cn.md)
