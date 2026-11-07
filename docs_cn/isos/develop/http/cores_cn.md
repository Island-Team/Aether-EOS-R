# Core API v1

[English](../../../../docs/isos/develop/http/cores.md) · [owner 边界](../owner/README_cn.md)

**IsOS 一侧已在当前 IsOS v1 代码中实现。** IsOS 报告所选 EOS/EGIA 就绪并调用其有界 setup 适配器。所有路由需要回环 Host/Origin 准入。共享 HTTP 封装为 `{schema_version:"0.1.0",ok,data,error}`；owner 不兼容或不可用返回结构化 503，而非空成功。

## GET /api/isos/v1/cores/readiness

返回 `data.cores`、图与 IsOS/UI 配置就绪。描述所选安装路径和兼容性，不证明所有端点正在监听。

## GET /api/isos/v1/cores/setup-status

返回 `data.ready`、`issues`、`graph_revision`、backend 选项及进程摘要。响应示例：`{"schema_version":"0.1.0","ok":true,"data":{"ready":false,"issues":["selected owner setup is unavailable"],"graph_revision":null,"backends":[],"process":null},"error":null}`。

## GET /api/isos/v1/cores/setup/{role}/catalog

`role` 只能为 `eos` 或 `egia`。IsOS 调用所选 setup 适配器的 模式 2 的 catalog 操作，不启动运行时，并在 `data` 返回有类型 owner 响应。

## POST /api/isos/v1/cores/setup/{role}/validate

请求 `{"schema_version":"0.1.0","graph":{"version":"0.1.0","cores":{}},"revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}`。IsOS 将草稿及修订传给所选 owner，不发布配置。owner 响应含数字 `schema_version:2`、`ok`、`diagnostics`、`files` 和可选 `identity` 或 `catalog`。

## POST /api/isos/v1/cores/setup/{role}/render

使用与 validate 相同的请求形状，只预览有界相对生成文件，不发布。缺图返回 400，未知 role 或操作返回 404，超时、畸形响应或 setup 身份未声明 `setup_schema_version:2` 返回 503。

`schema_version` 为字符串/—且必须为 `0.1.0`；`role` 是封闭字符串/—，防止选择任意可执行文件；`graph` 为已注册 profile 对象/—；`revision` 是可选 64 位小写 SHA-256 字符串，供已有保存字节围栏的调用者使用。外部 setup wire 使用模式 2 和有界 stdin/stdout；产品版本不代表 setup 兼容。

## 失败示例与字段契约

对于 `POST /api/isos/v1/cores/setup/eos/validate`，以下是结构化失败示例：

```json
{"schema_version":"0.1.0","ok":false,"data":null,"error":{"code":"owner_unavailable","message":"required owner is unavailable","details":null,"correlation_id":"req-42"}}
```

`role` 是 `eos` 或 `egia` 路径选择器；未知角色不能选择可执行文件。验证/渲染的 `graph` 是必需草稿对象；`revision` 是可选的精确已保存字节 SHA-256 栅栏；`schema_version` 必须为 `0.1.0`。Setup 适配器使用[独立契约](../../interfaces/cores/setup_cn.md)。 示例中的关联 ID 在每次失败时生成；通用封装字段和状态映射见 [HTTP](envelope_cn.md)。
