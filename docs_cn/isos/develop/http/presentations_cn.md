# 展示 HTTP 路由

本契约面向请求活动图某个输出表示形式的本地客户端。路由已实现，但当前没有归属方注册已验证的公开描述符，因此有效的当前请求返回 503。[数据类型](../../interfaces/ui/selection_cn.md)和[描述符](../../interfaces/ui/presentation_cn.md)契约定义内部选择边界。

## 认证与请求

`POST /api/isos/v1/presentations/select` 要求回环 Host/Origin 准入。它接受 `schema_version: "0.1.0"` 的 JSON 主体；未知字段和畸形 JSON 均失败。共享[信封](envelope_cn.md)定义错误和关联 ID。

| 字段 | 类型／单位 | 用途与验证 | 缺省值 |
| --- | --- | --- | --- |
| `schema_version` | 字符串／版本 | 必须为 `0.1.0`；数字值以 `unsupported_schema_version` 拒绝。 | 必填。 |
| `node_path` | 字符串／图路径 | 规范的四段输出归属路径；有界并需验证。 | 必填。 |
| `port` | 字符串／输出端口 ID | 标识一个输出；有界并需验证。 | 必填。 |
| `accept` | 非空数组／渲染方法 | 客户端支持的 `json`、`text`、`chart`、`image`、`canvas`、`overlay` 或 `audio`；顺序表示偏好。 | 必填。 |
| `run_id` | 字符串／运行身份 | 必须匹配活动运行。 | 必填。 |
| `graph_revision` | 字符串／SHA-256 | 必须匹配活动修订。 | 必填。 |

```json
{"schema_version":"0.1.0","node_path":"eos/interface/gate/front","port":"preview","accept":["image","canvas"],"run_id":"r7-cde","graph_revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}
```

## 当前结果

畸形选择返回 400 `invalid_request`；模式版本错误返回 400 `unsupported_schema_version`。过期的运行或修订返回 409 `revision_conflict`。当前请求如果没有注册描述符，则返回 503 `owner_unavailable`：

```json
{"schema_version":"0.1.0","ok":false,"data":null,"error":{"code":"owner_unavailable","message":"presentation owner has not registered a public delivery descriptor","details":null,"correlation_id":"req-43"}}
```

当前路由不返回成功信封或端点 ID。Stop、Restart 和 Apply 会使先前运行／修订的选择失效。生产者保留字节与端点授权。

[English](../../../../docs/isos/develop/http/presentations.md)
