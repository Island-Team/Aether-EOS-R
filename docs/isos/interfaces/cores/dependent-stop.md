# Dependent stop contract

[中文](../../../../docs_cn/isos/interfaces/cores/dependent-stop_cn.md) · [Design](../../design/server.md)

Current IsOS reads a strict, newline-terminated terminal-stop receipt from the selected owner pipe. The receipt uses schema `1`. IsOS binds the receipt to the selected `run_id`, owner PID, active revision, admitted package process node and expected parts. The owner proves its descendants and physical stop; IsOS independently proves its own direct process group exit. Missing, malformed, stale, late, or contradictory proof yields `unknown`.

| Receipt field | Type / unit | Purpose and validation | Absence/default |
| --- | --- | --- | --- |
| `schema_version`, `type` | integer `1`; string `eos_terminal_stop` / identity | Exact currently accepted owner receipt shape. | Required. |
| `run_id`, `active_revision` | strings / run and SHA-256 | Must match selected run and graph revision. | Required. |
| `eos_pid`, `backend_node` | integer; graph path / owner and node | Must match admitted owner process and backend node. | Required. |
| `backend_pid`, `backend_pgid` | integers or null / process IDs | Bind admitted descendant process group. | Required fields; values may be null. |
| `generation_id`, `stop_epoch` | strings or null / owner generation | Owner-issued stop correlation. | Required fields; values may be null. |
| `admission_closed`, `backend_group_drained`, `backend_reaped`, `sockets_removed` | booleans / stop facts | All required facts are checked before `stopped`. | Required. |
| `parts` | array / selected physical outcomes | Each entry gives `node_path`, registered hardware group as `kind`, and `stopped` or `unknown`; sorted paths must cover the admitted backend references exactly. | Required. |
| `outcome`, `reason_code` | string and optional string / terminal result | `unknown` remains conservative. | Required outcome. |

Restart cannot replace a run when physical stop remains uncertain.
