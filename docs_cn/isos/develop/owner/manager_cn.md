# 管理器 IPC v1

[English](../../../../docs/isos/develop/owner/manager.md) · [边界索引](README_cn.md) · [运行时 HTTP](../http/runtime_cn.md)

## 概览

IsOS CLI 和回环 HTTP 服务器通过 owner 专用 Unix socket 调用 IsOS 管理器。此 wire 为 `isos.manager@v1`，独立于 HTTP schema、图版本和 owner setup。管理器串行化部署写操作。

## 身份验证与传输

Socket 检查同 UID 对端身份。一个请求是一行有大小限制和截止时间的 JSON 对象；过大、畸形、超时或未知操作在分派前失败。私有 socket 不得投射为浏览器端点。

## 接口

请求包含 `schema_version:"0.1.0"`、snake-case `op` 和该操作的专属 `payload`。类型化载荷中的额外字段会被拒绝。停止请求示例：

```json
{"schema_version":"0.1.0","op":"deployment_stop","payload":{"deployment":"default","revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}
```

响应包含 `schema_version:"0.1.0"`、`ok`、操作特定 `data`，以及可选 `error`（稳定 `code`、安全 `message` 和 `correlation_id`）。错误示例：

```json
{"schema_version":"0.1.0","ok":false,"data":null,"error":{"code":"revision_conflict","message":"request conflicts with current state","correlation_id":"req-42"}}
```

| 字段 | 类型 / 单位 | 用途与验证 |
| --- | --- | --- |
| `schema_version` | 字符串 / 版本 | 必须为 `0.1.0`。 |
| `op` | 闭合 snake-case 字符串 / 无 | 选择一个操作和载荷类型；未知值拒绝。 |
| `payload.deployment` | slug 字符串 / 无 | 选择管理器拥有的部署；部署和追加日志操作必需。 |
| `payload.revision` | 可选字符串 / SHA-256 | 部署写操作的精确已保存字节栅栏；省略表示接受当前版本。 |
| `payload.service_id` | 字符串 / 无 | 仅服务目标和服务日志操作必需。 |
| `payload.cursor` | 可选不透明字符串 / 无 | 服务日志续读；客户端不解码。 |
| `payload.limit` | 可选无符号整数 / 条记录 | 限制服务日志读取。 |
| `payload.log` | 对象 / 一条事件 | `log_append` 的一条有界事件，含 `source`、`level`、`message` 和可选 `event_code`。 |

## 操作

无载荷操作：`manager_identity`、`manager_shutdown`、`services_list`、`services_validate`、`services_status`。部署操作：`deployment_start`、`deployment_restart`、`deployment_stop` 使用 `deployment` 和可选 `revision`；`deployment_status`、`deployment_active_packages` 仅使用 `deployment`。服务写操作 `services_start`、`services_stop`、`services_restart` 使用 `service_id`；`services_logs` 增加可选 `cursor` 和 `limit`。`log_append` 使用 `deployment` 和一条 `log`。在 `deployment_stop` 中添加 `service_id` 等跨操作字段无效。停止只在 owner 证明后确认；见[依赖停止](../../interfaces/cores/dependent-stop_cn.md)。
