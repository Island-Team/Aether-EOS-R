# North HTTP configuration module

[North HTTP process](README.md) · [Server](server.md) · [Repository configuration](../../../usage/configuration.md) · [中文](../../../../../docs_cn/eos/develop/ext/north_http/config_cn.md)

`src/eos/ext/north_http/config/` owns the `eos::ext::north_http::Config` value, strict `north-http.toml` parsing through `LoadConfig`, upstream validation through `ValidateUpstream`, and loopback admission through `IsLoopbackHost`.

## Sources and precedence

The parser reads one absolute file selected by root process composition. `--config` chooses that file; otherwise layout resolution selects `etc/eos/north-http.toml`. `LoadConfig` admits only root `[north_http]`, optional `[action_list_audit]`, and optional `[log]` tables and rejects an unreadable file, malformed TOML, unknown key, wrong type, or invalid value before listener construction.

After parsing, root `main.cpp` applies `--host`, `--port`, `--upstream`, and `--log-level` overrides. `--etc` and `--contracts` affect layout resolution rather than `Config`. Upstream and non-loopback safety checks run again after CLI overrides, so `--upstream unix:...` has the same absolute-path requirement as TOML.

## North HTTP keys

| Key | Default | Constraint and owner |
| --- | --- | --- |
| `host` | `127.0.0.1` | non-empty listener host; non-loopback requires `allow_remote` |
| `port` | `16005` | integer in `1..65535` |
| `upstream` | `127.0.0.1:16003` | non-empty EOS gRPC target; `unix:/absolute/path.sock` is accepted |
| `allow_remote` | `false` | explicit bind opt-in only; it adds no security mechanism |
| `allowed_origins` | Island production origins plus localhost and `127.0.0.1` port 3000 | array of non-empty exact origins |
| `max_request_bytes` | `67108864` | positive HTTP request-body bound |
| `max_connections` | `64` | positive total accepted-connection capacity, maximum `256` |
| `max_inflight_requests` | `32` | positive unary-request capacity, maximum `256` |
| `max_sse_streams` | `16` | positive SSE stream capacity, maximum `256` |
| `max_monitor_streams` | `4` | positive monitor WebSocket capacity, maximum `256` |
| `connect_timeout_ms` | `2000` | positive readiness probe deadline |
| `request_timeout_ms` | `630000` | positive unary gRPC deadline, at most `630000`; long-lived streams have no unary deadline |
| `sse_heartbeat_ms` | `15000` | positive SSE comment-heartbeat interval |

The three request-class capacities must sum to at most `max_connections`; validation performs this check without overflowing. The config module validates shape and scalar bounds. The server owns runtime enforcement. The client consumes only the admitted `upstream` string when it constructs the shared channel.

## ActionList audit keys

| Key | `Config` default | Constraint and owner |
| --- | --- | --- |
| `enabled` | `false` | constructs the external audit service when true; the committed reference file explicitly enables it |
| `rotation_size_bytes` | `16777216` | positive hard per-record and segment-line cap, maximum `268435456` |
| `rotation_files` | `8` | positive retained-segment count, maximum `64` |
| `reconnect_delay_ms` | `1000` | positive stream retry delay, maximum `60000` |

No audit directory key exists. Root composition derives `<state>/north-http/action-list-audit` from the resolved Island state root, applies the admitted limits to `audit::Config`, and starts the service before HTTP serving. The audit module owns runtime journal permissions, rotation, retention, recovery, and stream retry.

The configured `rotation_size_bytes * rotation_files` product must not exceed 256 MiB, independently of the scalar maxima.

## Logging keys

The optional strict `[log]` table accepts `level`, `file_level`, `jsonl_level`, `overwrite`, `dedup`, `dedup_window_s`, `save_attachments`, `rotation_size_bytes`, and `rotation_files`. String, boolean, and numeric types are checked here; the shared logging library completes log-level and logging-policy validation when root composition calls `ConfigureLogging`.

North HTTP uses the `north-http` service directory independently of `eosd`. Logging settings do not enable payload logging: server, process, and audit diagnostics remain metadata-only even when the JSONL threshold is verbose. `[log]` settings do not configure, rotate, deduplicate, overwrite, or attach content from the separate durable ActionList audit journal.

## Security and failure behavior

`IsLoopbackHost` accepts only `127.0.0.1`, `::1`, and `localhost`. `allow_remote = true` permits another bind address but does not configure TLS, authentication, authorization, rate limiting, or a public CORS policy.

Configuration failure is terminal and occurs before the client or listener is created. The process reports a bounded error to stderr and attempts to record `north_http_process_failed` through fallback logging; it never serves with a partially admitted configuration.

## Testing

`eos_north_http_config_tests` verifies key defaults, strict positive connection-capacity parsing, operational and aggregate capacity maxima, unknown-key and wrong-type rejection, maximum request-timeout rejection, remote-bind refusal without opt-in, the exact recognized loopback host set, strict audit keys, and audit operational maxima. Server and audit tests verify that admitted limits, origins, and journal settings are enforced at runtime.
