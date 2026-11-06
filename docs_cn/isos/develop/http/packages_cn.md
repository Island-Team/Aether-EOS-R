# 软件包 HTTP API

[English](../../../../docs/isos/develop/http/packages.md) · [封套](envelope_cn.md) · [通用软件包契约](../../interfaces/packages/README_cn.md)

所有路由要求回环 Host/Origin 准入。请求和响应使用 `schema_version:"0.1.0"`。安装、链接和预览不更改保存的图或活动运行。软件包 ID 是数据，不是解析器开关。

| 方法和路由 | 输入 | 成功 | 失败 |
| --- | --- | --- | --- |
| `GET /api/isos/v1/packages` | 无正文 | 精确锁定和验证状态的 `data.installed` 行。 | 403 Host/Origin；503 存储错误。 |
| `POST /api/isos/v1/packages/install` | 精确 `id`、`version`；可选已批准目录选择。 | 已验证收据、路径、更新的清单。 | 400 格式错误；409 锁定重复；422 契约不支持或工件不安全。 |
| `POST /api/isos/v1/packages/link` | 精确 `id`、`version`、绝对 `path`，可选绝对 `manifest`。 | 已验证链接收据和清单。 | 400 格式错误；409 锁定重复；422 不受支持或载荷变化。 |
| `POST /api/isos/v1/packages/preview-link` | 绝对软件包 `path`，可选 `manifest`。 | `id`、`version`、`ready`、有界 `issues`；不发布。 | 400 格式错误；422 软件包不安全。 |

```json
{"schema_version":"0.1.0","id":"vendor/sample-a","version":"1.2.3","path":"/absolute/bundle","manifest":"/absolute/bundle/release.json"}
```

链接成功返回 `{"schema_version":"0.1.0","ok":true,"data":{"verified":true,"source_kind":"local_link"},"error":null}`，另有契约定义的收据和清单字段。缺少必要注册声明的贡献无法通过准入。移除软件包拒绝保存或活动的精确锁定引用。发行包、收据、资源和诊断的字段以[软件包接口](../../interfaces/packages/README_cn.md)为准。
