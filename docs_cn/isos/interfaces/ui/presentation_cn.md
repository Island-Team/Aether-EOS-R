# 展示描述符边界

本页向归属方和客户端实现者说明纯选择器使用的有类型描述符。描述符标识注册输出及一种表示形式；它不携带负载字节或端点授权。当前 HTTP 路由没有已验证的描述符注册，在输入与运行检查后返回 503。

| 字段 | 类型／单位 | 用途与验证 | 缺省值 |
| --- | --- | --- | --- |
| `node_path`、`port` | 字符串／图路径与端口 ID | 标识一个输出；都必须是有效且有界的标识。 | 必填。 |
| `data_type` | `DataType`／语义类型 | 提供选择所需的编码、媒体类型、结构及负载上限。 | 必填。 |
| `representation_id` | 字符串／标识 | 区分同一输出的不同生产者表示。 | 必填。 |
| `method` | 枚举／渲染方式 | `json`、`text`、`chart`、`image`、`canvas`、`overlay`、`audio` 之一。 | 必填。 |
| `delivery` | 枚举／传输 | `http` 或 `web_socket`；不授予访问权。 | 必填。 |
| `endpoint_id` | 字符串／不透明键 | 标识生产者路由；不是 URL 或私有授权。 | 必填。 |
| `run_id`、`graph_revision` | 字符串／运行 ID 与 SHA-256 | 将描述符限定在一个活动运行和图修订。 | 必填。 |

纯[选择器](selection_cn.md)检查字段形状、编码／媒体／方法匹配、负载上限及所请求的运行／修订。注册端口与类型的绑定由上游归属方准入负责；当前 HTTP 路由无法执行实时描述符选择。[HTTP 契约](../../develop/http/presentations_cn.md)定义当前请求和错误。

[English](../../../../docs/isos/interfaces/ui/presentation.md)
