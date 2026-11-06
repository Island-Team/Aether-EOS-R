# 软件包发行契约

[English](../../../../docs/isos/interfaces/packages/release.md) · [验证](verification_cn.md) · [资源](resources_cn.md)

`isos.package@v1` 是活动的 IsOS 贡献身份。发行清单使用 `"schema_version":"0.1.0"`，`node_contributions` 是注册节点类型、进程、资源、节点集的唯一活动来源。IsOS 仅通过精确 `{id,version}` 锁定读取发行包。选中类型在 Apply、Run 或 Restart 前必须有经过验证的通用贡献。

| 字段 | 类型 / 单位 | 用途与验证 | 缺省值 |
| --- | --- | --- | --- |
| `contract` | 字符串 / 身份 | 必须为 `isos.package@v1`。 | 必填。 |
| `schema_version` | 字符串 / 版本 | 必须为 `0.1.0`。 | 必填。 |
| `id`, `version` | 字符串 / 精确软件包锁定 | 规范包 ID 和精确发行版本；不推断最新版。 | 必填。 |
| `kind` | 字符串 / 目录类别 | 用于发现的有界稳定描述符；仅为元数据，不选择行为或解析器。 | 必填。 |
| `source_commit` | 40 位十六进制字符串 / Git 提交 | 发布来源；独立于载荷摘要。 | 必填。 |
| `description`, `license`, `source_url`, `release_url` | 字符串 / 显示文本或 URL | 仅用于目录元数据；不能授予运行时能力。 | 可选，缺省为空。 |
| `contracts` | 字符串数组 / 身份 | 声明外部能力；所有者准入仍需验证。 | 可选，缺省空数组。 |
| `node_contributions` | 对象 / 声明 | 注册类型、进程、数据类型和节点集；IsOS 验证结构与所有权。 | Apply/Run 必需。 |
| `assets` | 数组 / 候选工件 | 按主机匹配的有界下载候选，包含 URL、字节长度和 SHA-256。 | 归档安装必需。 |

发行包可在有界的所有者扩展中包含不透明元数据。IsOS 不把这些元数据解释为场景、设备、运行库或适配器权限。所属核心在宣布就绪前验证其语义。[进程契约](process_cn.md)定义通用贡献字段。

```json
{"contract":"isos.package@v1","schema_version":"0.1.0","id":"vendor/sample-a","kind":"component","version":"1.2.3","source_commit":"0000000000000000000000000000000000000000","node_contributions":{"node_types":[],"processes":[],"node_sets":[]},"assets":[]}
```

空列表示例只说明结构；准入还要求选中的注册类型和声明的所有者能力。[CLI 发布流程](../../maintenance/todo_cn.md)与软件包准入分开。
