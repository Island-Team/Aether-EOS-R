# 选中核心设置适配器

[English](../../../../docs/isos/interfaces/cores/setup.md) · [核心 HTTP](../../develop/http/cores_cn.md)

已实现的外部所有者设置交换使用**模式 2**。IsOS 选择配置的所有者可执行程序；客户端不能提交路径。输入上限 1 MiB，输出上限 4 MiB，截止时间 10 秒。`identity` 与 `catalog` 只读；`validate` 检查草稿；`render` 预览有界生成文件。IsOS 在发布前拒绝错误核心、不兼容身份、格式错误或过大响应、越界路径及超时。此外部通信格式独立于 IsOS 自有 HTTP `"0.1.0"` 模式。

```json
{"schema_version":2,"ok":true,"diagnostics":[],"files":[],"identity":{"core":"eos","setup_schema_version":2,"product_version":"0.2.0","graph_version":"0.1.0","node_type_version":"0.1"}}
```

| 字段 | 类型 / 单位 | 用途与验证 | 缺省值 |
| --- | --- | --- | --- |
| `schema_version` | 整数 / 外部所有者模式 | 请求与响应均精确为 `2`。 | 必填。 |
| `op` | `identity`、`catalog`、`validate`、`render` / 操作 | 选择一个有界操作。 | 请求必填。 |
| `core` | 字符串 / 所有者角色 | 必须匹配选中配置的所有者。 | 请求必填。 |
| `graph`, `revision` | JSON 对象与可选 64 位十六进制 / 图及 SHA-256 | 验证/渲染草稿及可选保存字节栅栏。 | 依操作而定。 |
| `ok`, `diagnostics`, `files` | 布尔值与数组 / 结果 | 封闭响应；诊断条目有 `path`、`code`、`message`；文件条目有安全相对 `path`、`content`。 | 响应必填。 |
| `identity`, `catalog` | JSON 或 null / 所有者元数据 | 按操作提供；身份具有 `setup_schema_version:2`。 | 可选。 |
