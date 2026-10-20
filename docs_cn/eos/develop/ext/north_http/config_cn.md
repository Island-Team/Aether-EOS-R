# North HTTP configuration module

[North HTTP 进程](README_cn.md) · [Server](server_cn.md) · [仓库配置](../../../usage/configuration_cn.md) · [English](../../../../../docs/eos/develop/ext/north_http/config.md)

`src/eos/ext/north_http/config/` 拥有 `eos::ext::north_http::Config` value、通过 `LoadConfig` 执行的 strict `north-http.toml` parsing，以及通过 `IsLoopbackHost` 执行的 loopback admission。

## Source 与 precedence

parser 读取一个由 root process composition 选择的 absolute file。`--config` 选择该 file；否则 layout resolution 选择 `etc/eos/north-http.toml`。`LoadConfig` 仅接纳 root `[north_http]`、optional `[action_list_audit]` 与 optional `[log]` table，并在 listener construction 前拒绝 unreadable file、malformed TOML、unknown key、wrong type 或 invalid value。

parsing 后，root `main.cpp` 应用 `--host`、`--port`、`--upstream` 与 `--log-level` override。`--etc` 与 `--contracts` 影响 layout resolution，而不是 `Config`。non-loopback safety check 在 CLI override 后再次运行。

## North HTTP key

| Key | Default | Constraint and owner |
| --- | --- | --- |
| `host` | `127.0.0.1` | non-empty listener host；non-loopback 需要 `allow_remote` |
| `port` | `16005` | `1..65535` 内的 integer |
| `upstream` | `127.0.0.1:16003` | non-empty EOS gRPC target |
| `allow_remote` | `false` | 仅是 explicit bind opt-in；它不添加 security mechanism |
| `allowed_origins` | Island production origin 加 localhost 与 `127.0.0.1` port 3000 | non-empty exact origin 的 array |
| `max_request_bytes` | `67108864` | positive HTTP request-body bound |
| `max_connections` | `64` | positive accepted-connection 总容量，最大 `256` |
| `max_inflight_requests` | `32` | positive unary-request 容量，最大 `256` |
| `max_sse_streams` | `16` | positive SSE stream 容量，最大 `256` |
| `max_monitor_streams` | `4` | positive monitor WebSocket 容量，最大 `256` |
| `connect_timeout_ms` | `2000` | positive readiness probe deadline |
| `request_timeout_ms` | `630000` | positive gRPC call/stream deadline，最大为 `630000` |
| `sse_heartbeat_ms` | `15000` | positive SSE comment-heartbeat interval |

三个 request-class capacity 的总和不得超过 `max_connections`，且 validation 以 overflow-safe 方式执行该检查。config module 验证 shape 与 scalar bound。server 拥有 runtime enforcement。client 在构造 shared channel 时只消费 admitted `upstream` string。

## ActionList audit key

| Key | `Config` default | Constraint and owner |
| --- | --- | --- |
| `enabled` | `false` | 为 true 时构造 external audit service；已提交的 reference file 显式启用它 |
| `rotation_size_bytes` | `16777216` | positive hard per-record 与 segment-line cap，最大 `268435456` |
| `rotation_files` | `8` | positive retained-segment count，最大 `64` |
| `reconnect_delay_ms` | `1000` | positive stream retry delay，最大 `60000` |

不存在 audit directory key。root composition 从 resolved Island state root 派生 `<state>/north-http/action-list-audit`，把 admitted limit 应用到 `audit::Config`，并在 HTTP serving 前启动 service。audit module 拥有 runtime journal permission、rotation、retention、recovery 与 stream retry。

配置的 `rotation_size_bytes * rotation_files` product 必须不超过 256 MiB，这一约束独立于 scalar maximum。

## Logging key

optional strict `[log]` table 接受 `level`、`file_level`、`jsonl_level`、`overwrite`、`dedup`、`dedup_window_s`、`save_attachments`、`rotation_size_bytes` 与 `rotation_files`。string、boolean 与 numeric type 在此检查；root composition 调用 `ConfigureLogging` 时，shared logging library 完成 log-level 与 logging-policy validation。

North HTTP 独立于 `eosd` 使用 `north-http` service directory。logging setting 不会启用 payload logging；即使 JSONL threshold 很详细，server、process 与 audit diagnostic 仍只包含 metadata。`[log]` setting 不配置、rotate、deduplicate、overwrite 或 attach 独立 durable ActionList audit journal 的 content。

## Security 与 failure behavior

`IsLoopbackHost` 仅接受 `127.0.0.1`、`::1` 和 `localhost`。`allow_remote = true` 允许其他 bind address，但不配置 TLS、authentication、authorization、rate limiting 或 public CORS policy。

configuration failure 是 terminal 的，且发生在 client 或 listener 创建前。process 向 stderr 报告 bounded error，并尝试通过 fallback logging 记录 `north_http_process_failed`；它绝不会使用 partially admitted configuration 提供服务。

## 测试

`eos_north_http_config_tests` 验证 key default、strict positive connection-capacity parsing、operational 与 aggregate capacity maximum、unknown-key 与 wrong-type rejection、maximum request-timeout rejection、未 opt-in 时的 remote-bind refusal、精确的 recognized loopback host set、strict audit key 与 audit operational maximum。Server 与 audit test 验证 admitted limit、origin 与 journal setting 在 runtime 得到强制。
