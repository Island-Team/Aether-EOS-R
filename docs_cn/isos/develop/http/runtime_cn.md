# 运行时 API v1

[English](../../../../docs/isos/develop/http/runtime.md) · [停止证明](../../interfaces/cores/dependent-stop_cn.md) · [日志游标](../owner/log-cursors_cn.md)

**IsOS 侧已实现。** IsOS 负责部署命令准入、状态和有界日志读取。所有路由需要回环 Host/Origin 准入。统一 HTTP 封装为 `{schema_version:"0.1.0",ok,data,error}`；错误包含稳定 `code`、安全 `message` 与 `correlation_id`。命令等待管理器响应；没有 owner 证明时，停止结果不得称为已确认。

## GET /api/isos/v1/runtime/process-status

返回管理器状态，包括保存及活动修订、进程状态、run ID 和保留的终态结果。响应形状示例：`{"schema_version":"0.1.0","ok":true,"data":{"state":"stopped","run_id":null,"processes":[],"packages":[]},"error":null}`。`packages` 是精确 `{id,version,path}` 图锁定数组；空数组表示未选择锁定。其他健康字段见[状态契约](../../interfaces/cores/dependent-stop_cn.md)。

## POST /api/isos/v1/runtime/control

请求 `{"schema_version":"0.1.0","operation":"stop","revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}`。`operation` 是 `run`、`start`、`stop` 或 `restart`；`revision` 是可选的准确保存字节 SHA-256 围栏。成功 `data` 是管理器操作结果。过期修订返回 409；owner 失败或停止不确定返回结构化故障，不能声称成功。此路由不能绕过 EOS 执行器准入。

## GET /api/isos/v1/runtime/logs

通过 `source`、`cursor`、`limit`、`level` 和可选当前运行 `view` 查询有界脱敏 JSONL。请求示例：`GET /api/isos/v1/runtime/logs?source=all&limit=100&level=info`；`data` 含 `lines`、`next_cursor` 和 `rotated`。每次至多 200 条与 64 KiB；轮转游标明确报告。当前运行锚点及间隙诊断见[日志游标](../owner/log-cursors_cn.md)。

`schema_version` 为字符串/—，变更时必须为 `0.1.0`；`operation` 为字符串/—且必填；`revision` 为可选字符串/SHA-256，仅调用者接受当前保存修订时可省；`cursor` 为不透明字符串/—，客户端不解析；`level` 为五种共享严重级别之一。这些字段区分保存编辑与活动运行，并限制日志读取。停止、就绪及错误可通过管理器结果和部署事件流观察。

## 失败示例与字段契约

对于 `POST /api/isos/v1/runtime/control`，以下是结构化失败示例：

```json
{"schema_version":"0.1.0","ok":false,"data":null,"error":{"code":"invalid_request","message":"operation must be run, start, stop, or restart","details":null,"correlation_id":"req-42"}}
```

`operation` 必须是 `run`、`start`、`stop` 或 `restart`；`run` 与 `start` 选择同一管理器启动操作。`revision` 是可选精确已保存字节栅栏。`cursor` 不透明，`limit` 是有上限的记录数。控制成功响应将管理器操作数据放入共享封装。 示例中的关联 ID 在每次失败时生成；通用封装字段和状态映射见 [HTTP](envelope_cn.md)。
