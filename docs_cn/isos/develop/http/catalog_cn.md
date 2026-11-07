# 目录 HTTP API

[English](../../../../docs/isos/develop/http/catalog.md) · [封套](envelope_cn.md) · [图](../../interfaces/nodes/README_cn.md)

目录路由公开注册节点类型、选中类型、节点选项及节点集预览。结果来自经过验证的通用 `node_contributions` 和选中所有者目录；软件包 ID 不改变解析或优先级。所有路由都需要回环 Host/Origin 准入。

| 路由 | 结果 | 状态影响 |
| --- | --- | --- |
| `GET /api/isos/v1/catalog/node-types` | 注册类型及可用性。 | 无。 |
| `GET /api/isos/v1/catalog/node-types/selected` | 已应用图引用的类型。 | 无。 |
| `POST /api/isos/v1/catalog/node-options` | 声明设置与稳定资源 ID 的选项。 | 无。 |
| `GET /api/isos/v1/catalog/node-sets` | 经过验证的声明节点集。 | 无。 |
| `POST /api/isos/v1/catalog/node-sets/preview` | 候选图组合；不发布。 | 无。 |

需要正文的请求使用 `schema_version:"0.1.0"`。格式错误的模式返回 400；未知精确锁定、类型、资源或所有者返回有界准入诊断。过期草稿修订返回 409。字段身份与验证细节见[图](../../interfaces/nodes/instance_cn.md)、[选项](../../interfaces/nodes/selectors_cn.md)和[资源](../../interfaces/packages/resources_cn.md)。
