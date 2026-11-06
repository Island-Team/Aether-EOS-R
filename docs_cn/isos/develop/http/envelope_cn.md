# 本地 HTTP 封套 v1

[English](../../../../docs/isos/develop/http/envelope.md) · [接口目录](../../interfaces/README_cn.md)

## 概览

回环 API 在 `/api/isos/v1/` 下提供八个域。每个处理器返回 JSON 封装。各域页面定义路由载荷；本页定义通用传输规则。API schema 独立于 IsOS 产品、图、包和 owner 协议版本。

## 请求准入

每条路由在操作前检查回环 `Host` 和提供的 `Origin`。禁止的来源或主机返回 403。当前本地路由不要求 owner bearer token。回环传输不授权访问 EOS 或 EGIA 私有 socket。

## 接口

成功响应含 `schema_version:"0.1.0"`、`ok:true`、域特定 `data` 及 `error:null`。以下为系统路由实现的示例：

```json
{"schema_version":"0.1.0","ok":true,"data":{"api_schema_version":"0.1.0","isos_version":"0.1.0"},"error":null}
```

失败响应含 `schema_version:"0.1.0"`、`ok:false`、`data:null` 和结构化错误。以下 `correlation_id` 仅为示例；服务器为每次失败生成标识。

```json
{"schema_version":"0.1.0","ok":false,"data":null,"error":{"code":"invalid_request","message":"invalid request","details":null,"correlation_id":"req-42"}}
```

| 字段 | 类型 / 单位 | 用途与验证 |
| --- | --- | --- |
| `schema_version` | 字符串 / 版本 | 必需的 wire 区分字段，必须为 `0.1.0`；产品版本不能替代。 |
| `ok` | 布尔值 / 无 | 仅操作成功时为 true。 |
| `data` | JSON 值或 null / 由域定义 | 域结果；失败时为 null。 |
| `error` | 对象或 null / 无 | 成功时为 null；失败时为结构化错误。 |
| `error.code` | 字符串 / 无 | 稳定的机器可读类别或域诊断。 |
| `error.message` | 字符串 / 无 | 安全的人类可读消息；不可作为协议字段解析。 |
| `error.details` | JSON 值或 null / 由域定义 | 可选的有界诊断；null 表示未提供。 |
| `error.correlation_id` | 字符串 / 无 | 服务器生成的标识，用于关联净化后的日志。 |

畸形 JSON 和无效输入使用 400，Host/Origin 拒绝使用 403，资源缺失使用 404，版本冲突使用 409，准入失败使用 422，owner 不可用使用 503。域可以返回更具体的稳定 `error.code`；客户端按代码而非消息分支。要求 `schema_version` 的 JSON 写操作在发布前拒绝其他值。声明闭合请求结构的路由会拒绝未知字段；客户端不可假定所有路由的未知字段策略相同。

## 路由

[版本](version_cn.md)、[图](graph_cn.md)、[目录](catalog_cn.md)、[包](packages_cn.md)、[core](cores_cn.md)、[运行时](runtime_cn.md) 与[呈现](presentations_cn.md)定义各自载荷和副作用。[节点事件契约](../../interfaces/ui/node-events_cn.md)定义第八个域。路由器是方法/路径对的真实来源。
