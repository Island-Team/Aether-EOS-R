# 版本 API

[English](../../../../docs/isos/develop/http/version.md)

**IsOS 侧已实现。** IsOS 负责本地兼容身份。路由需要回环 Host/Origin 准入。它只读，不探测或启动其他 core。错误使用共享 schema-0.1.0 结构化 HTTP 封装。

## GET /api/isos/v1/system/version

返回独立的 API schema、IsOS 包版本及内置的本地版本支持映射。响应示例：`{"schema_version":"0.1.0","ok":true,"data":{"api_schema_version":"0.1.0","isos_version":"0.2.1","version_support":{"schema":"isos.version-support@v1","product_version":"0.2.1","target_graph_format":"0.2.0","isos_http_schema":"0.1.0","package_contract":"isos.package@v1","package_schema":"0.1.0","external_packages":[{"id":"island/mujoco-sim","version":"0.1.1","manifest_sha256":"c421039753c24ffc74658d6b1dfe5c9de6a1ed77b64df8fbaee77063da837096","asset_sha256":"bb2862b01c3ccab8b68570e766a0ad26c8c5783b3c87cf99225eb0edb7a260f4"}]}},"error":null}`。版本映射由 `config/version-support.toml` 编入程序，不访问 GitHub。客户端拒绝不支持的 `api_schema_version`；产品版本不能替代通信契约兼容检查。Host/Origin 被拒绝时返回安全的结构化 403 错误。

| 字段 | 类型／单位 | 用途与验证 | 缺省／默认 |
| --- | --- | --- | --- |
| `api_schema_version` | 字符串／契约版本 | 当前 IsOS HTTP 封装 schema，精确为 `0.1.0`。 | 必需。 |
| `isos_version` | 字符串／产品 SemVer | 已安装的 IsOS 源包版本；与 HTTP 独立。 | 必需。 |
| `version_support.schema` | 字符串／身份 | 内置映射契约，`isos.version-support@v1`。 | 必需。 |
| `version_support.product_version` | 字符串／产品 SemVer | 必须与编译的 IsOS 包版本一致。 | 必需。 |
| `version_support.target_graph_format` | 字符串／图格式 | 此构建的目标图格式，`0.2.0`。 | 必需。 |
| `version_support.isos_http_schema` | 字符串／契约版本 | 必须与响应的 HTTP schema 一致。 | 必需。 |
| `version_support.package_contract`、`.package_schema` | 字符串／软件包契约 | 当前软件包读取器接受的精确清单身份与 schema。 | 必需。 |
| `version_support.external_packages[]` | 数组／精确固定版本 | 经审核的外部软件包 ID、版本和 64 位十六进制清单／资产 SHA-256。 | 无软件包时为空。 |

`isos version check --json` 包含同一个 `version_support.map` 和本地图状态：目标格式为 `compatible`，受支持的 0.1.0 输入为 `migration_required`，其他格式为 `unsupported`，未保存图时为 `not_configured`。这只报告本地身份，不在线检查最新发布，也不自动迁移图。显式在线更新检查见[目标包管理器设计](../../design/package-manager-target_cn.md)。

## 失败示例与字段契约

对于 `GET /api/isos/v1/system/version`，以下是结构化失败示例：

```json
{"schema_version":"0.1.0","ok":false,"data":null,"error":{"code":"unauthorized","message":"unauthorized","details":null,"correlation_id":"req-42"}}
```

此路由无请求体或查询字段。示例中的关联 ID 在每次失败时生成；通用封装字段和状态映射见 [HTTP](envelope_cn.md)。
