# Rust v2 process spine

[中文](../../../../docs_cn/eos/design/runtime/process_cn.md) · [Migration plan](foundation.md) · [Node interface](../../interfaces/hardware.md)

`aether-runtime` is the Rust process selected by the public `eosd` launcher. It accepts one validated `1.0.0` node profile, creates one isolated adapter for each EOS hardware node, opens an exclusive durable command journal, and then binds a local Unix socket. It announces readiness only after all of those steps succeed. Built-in adapters are `mock-cxx` and `mujoco-v2`; physical ROS 2 and vendor devices are not yet admitted.

The process owns admission, command history, node health, and shutdown for its mock nodes. The unified profile supplies identity, location, capabilities, limits, and adapter selection. The dispatcher is the sole command route; the socket handler does not call adapters directly. An invalid profile, unsupported adapter, incomplete admission, unavailable journal, or socket bind error fails startup without a READY announcement. A signal or shutdown request stops admitted actuators and removes the socket.

## Startup and local control fields

| Field | Type | Purpose and rule |
| --- | --- | --- |
| `--profile` | UTF-8 file path | Supplies one bounded unified node profile; its root `version` must be `"1.0.0"`. |
| `--journal-dir` | directory path | Holds durable command reservations and outcomes; one process holds its exclusive lock. Keep it on persistent storage for restart testing. |
| `--socket` | Unix socket path | Local control endpoint; bind failure prevents readiness. Access to this path must be limited by local filesystem permissions. |
| READY `version` | string | Identifies the process protocol as `"1.0.0"`. |
| READY `status` | string | `"ready"` only after profile, adapters, journal, and socket have been admitted. |
| READY `socket` | string | Bound control socket path. |
| READY `hardware_nodes` | unsigned integer | Count of EOS hardware nodes registered for dispatch; software nodes remain in the profile but do not receive adapters. |
| request `op` | string | Selects `discover`, `health`, `time`, `dispatch`, `result`, `stop`, or `shutdown`. |
| request `node_path` | string, required for `health`, `dispatch`, and `stop` | Full `cores/.../nodes/...` path. Short IDs and software paths cannot control hardware. |
| request `command_id` | string, required for `dispatch` and `result` | Durable idempotency key; replay an identical request to retrieve its prior outcome. |
| request `deadline_monotonic_ns` | unsigned 64-bit integer, required for `dispatch` | Absolute process-local deadline. Obtain current time with `time` and submit before expiry. Replay the exact original value after restart. |
| request `movement` | tagged object, required for `dispatch` | `body_velocity`, `relative_pose`, `joint_positions`, or `end_effector_pose`, with [typed hardware fields](../../interfaces/hardware.md). The profile must admit its mode and limits. |
| response `ok` | boolean | Indicates whether the operation completed. |
| response `nodes` | array, on `discover` success | Hardware node descriptors derived from the admitted profile. |
| response `health` | object, on `health` success | Effective adapter health, including Rust-side observation faults. |
| response `monotonic_ns` | unsigned 64-bit integer, on `time` success | Current process-local time for calculating a command deadline; do not compare it with another process's clock. |
| response `result` | object, on `dispatch` or `result` success | Durable command ID, outcome status, and optional reason. A rejected command is still a recorded dispatch result. |
| response `error` | string, on failure | Human-readable validation or operation failure. Do not parse it as a stable error code. |

The socket uses one bounded newline-delimited JSON request and response per connection. The limit is 64 KiB per request. This surface is for local integration and lifecycle testing; the planned public v2 node API is a later milestone. `dispatch` uses the same full path, typed movement, deadline, and durable journal as the dispatcher. Replay the same semantic fields, including the absolute deadline; the journal prevents a second actuator release after restart.

## Proof and next gate

Process tests must exercise profile rejection, unsupported adapters, duplicate journal ownership, ready state, discovery, a mock command, result lookup, stop, shutdown, signal handling, and restart without command replay. Record output under `logs/test/<run-id>/`. Physical deployment still requires independent stop and velocity-lease expiry proof at the real controller. The old C++ process source remains reachable from the opt-in side chain, but no default build or install uses it; delete it after the last hardware-side caller moves.
