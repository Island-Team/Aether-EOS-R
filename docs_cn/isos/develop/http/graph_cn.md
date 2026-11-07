# 图 API v1

[English](../../../../docs/isos/develop/http/graph.md) · [图模型](../../interfaces/nodes/instance_cn.md)

**IsOS 侧已实现。** IsOS 负责保存图和准确字节修订。所有路由需要回环 Host/Origin 准入。JSON 变更需要 `schema_version:"0.1.0"`；Apply 检查修订。统一成功响应为 `{"schema_version":"0.1.0","ok":true,"data":{},"error":null}`。错误使用 `ok:false`、`data:null` 以及带稳定 `code`、安全 `message`、`correlation_id` 的对象；准入诊断可位于 `details`。输入错误为 400、缺少保存数据为 404、过期修订为 409、准入错误为 422。

## GET /api/isos/v1/graph

只读 `{revision,graph,read_only,diagnostics}`，不修改保存字节。图不存在时返回可编辑草稿和空修订。示例响应：`{"schema_version":"0.1.0","ok":true,"data":{"revision":"","graph":{"version":"0.1.0","cores":{}},"read_only":false,"diagnostics":[]},"error":null}`。

## PUT /api/isos/v1/graph

应用完整图。`revision` 是准确保存字节 SHA-256，图不存在时为 `""`；`graph` 是 JSON 对象。示例请求：`{"schema_version":"0.1.0","revision":"","graph":{"version":"0.1.0","cores":{}}}`。过期修订返回 `{"schema_version":"0.1.0","ok":false,"data":null,"error":{"code":"revision_conflict","message":"graph revision changed","details":null,"correlation_id":"req-42"}}`，保存图及活动子进程不变。

## POST /api/isos/v1/graph/validate

检查草稿而不写入：`{"schema_version":"0.1.0","graph":{"version":"0.1.0","cores":{}}}`。有效结果的 `data` 含 `{"valid":true,"diagnostics":[]}`；诊断使用稳定代码及 JSON Pointer 路径。

## POST /api/isos/v1/graph/migration/preview

只读保存的图。请求 `{"schema_version":"0.1.0","revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}`；响应 `data` 含 `revision`、`proposed_revision`、`graph`、`changes`、`can_apply`、`diagnostics`。

## POST /api/isos/v1/graph/migration/apply

准确提交先前预览的图。请求 `{"schema_version":"0.1.0","revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","proposed_revision":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}`；响应 `data` 含 `revision`、`graph`、`active_revision`。任一摘要变化均在发布前返回 409。

`schema_version` 为字符串/—，必须为 `0.1.0`；`revision` 与 `proposed_revision` 为字符串/SHA-256，必须是 64 位小写十六进制，唯独图不存在时的修订可为空；`graph` 为对象/—，遵循已注册节点 profile；`changes` 为数组/—，含有界 JSON Pointer 替换。这些字段区分草稿、保存图和活动运行；变更操作不推测缺省图修订。

## 失败示例与字段契约

对于 `PUT /api/isos/v1/graph`，以下是结构化失败示例：

```json
{"schema_version":"0.1.0","ok":false,"data":null,"error":{"code":"revision_conflict","message":"request conflicts with current state","details":null,"correlation_id":"req-42"}}
```

`revision` 是已保存字节的精确 SHA-256，仅图不存在时可为空；`proposed_revision` 精确标识已预览迁移输出。版本不匹配不会发布图或替换子进程。`diagnostics` 条目包含稳定代码与 JSON Pointer 路径。 示例中的关联 ID 在每次失败时生成；通用封装字段和状态映射见 [HTTP](envelope_cn.md)。
