# EOS v1 图设置适配器

[English](../../../docs/eos/interfaces/setup-v1.md) · [节点 profile](hardware_cn.md)

IsOS 在启动 EOS 之前调用选定的 `aether-profile setup --json` 程序。适配器从标准输入读取一条 JSON 请求，使用 EOS 注册的节点类型验证共享 v1 图，并向标准输出写入一条 JSON 响应。它不监听端口、不打开硬件、不启动 EOS，也不写入部署文件。软件包验证、图修订检查和返回文件的原子安装由 IsOS 负责。`16021` 上的运行时目录仍是独立的只读端点。

请求上限为 1 MiB。IsOS 通过 `packages: [{id, version, path}]` 传入软件包贡献的节点类型侧文件位置，其中 `path` 是已验证软件包目录的绝对路径。对于 `validate` 和 `render`，EOS 读取各软件包存在的 `node-types.json`，检查文件位于对应软件包目录内、身份与请求一致，并在验证图之前注册贡献的类型和适配器族。直接调用 CLI 时仍可重复使用 `--node-types <verified-json>` 参数。`endpoints` 是 IsOS 解析后的端点映射。`revision` 必须是长度为 1 至 128 字节的 ASCII 字符串。

```json
{"schema_version":1,"op":"render","core":"eos","revision":"af44f1","graph":{"version":"1.0.0","robot":{"id":"robot","name":"Robot"},"cores":{}},"endpoints":{},"packages":[]}
```

`validate` 返回诊断，不返回文件。`render` 将验证后的规范图作为相对于 IsOS 托管配置根目录的 `eos/graph.json` 返回。IsOS 必须拒绝不安全的路径，并且只在已保存图仍匹配请求修订时安装该文件。包含 JSON 转义文件内容的响应上限为 4 MiB。

```json
{"schema_version":1,"ok":true,"revision":"af44f1","diagnostics":[],"files":[{"path":"eos/graph.json","content":"{\"version\":\"1.0.0\",\"robot\":{\"id\":\"robot\",\"name\":\"Robot\"},\"cores\":{}}"}]}
```

`catalog` 只需要 `schema_version`、`op` 和 `core`，返回含 `types`、`data_types`、`adapter_families` 和 `primitive_specs` 的 `catalog`，`files` 为空数组。无效请求和图返回 `ok: false`、包含 `path`、`code`、`message` 的诊断以及空文件数组。此时退出码仍为零，使 IsOS 可以显示诊断；输入输出故障则以非零状态退出。

`identity` 同样只需要 `schema_version`、`op` 和 `core`。响应的 `diagnostics` 与 `files` 为空，并包含 `identity: {core, product_version, graph_version, node_type_version, setup_schema_version}`。`product_version` 来自 EOS 二进制程序的 Cargo 软件包版本；IsOS 用此操作报告已配置核心构建的版本，无需选择 Git 提交。`catalog` 中软件包贡献的适配器族标记为 `loadable: true`，运行时必须提供 IsOS 已验证的共享库。

```json
{"schema_version":1,"ok":true,"diagnostics":[],"files":[],"identity":{"core":"eos","product_version":"0.2.0","graph_version":"1.0.0","node_type_version":"1.0","setup_schema_version":1}}
```

这些版本分别标识 EOS 产品、共享图、节点类型目录和设置交互。EOS `/v2/eos` 控制路由、MuJoCo 套接字协议、`island/mujoco-sim@0.1.1` 软件包及持久化命令日志保留各自独立的版本。

## G05 身份与 P05 读取特性

G05 适配器目前返回 `supported_graph_versions:["1.0.0","0.1.0"]` 和 `supported_node_type_versions:["1.0","0.1"]`，同时保留旧标量 `graph_version:"1.0.0"`、`node_type_version:"1.0"` 与 schema 1。数组存在时具有权威性；后缀不授权任意完整 ID。未带 `--node-types` 的 `catalog` 返回基础注册表，重复使用 CLI 参数 `--node-types <verified-json>` 则会在处理请求前加入软件包贡献。请求中的 `packages` 数组由 `validate` 与 `render` 使用，不由提前返回的 `catalog` 回复使用。

P05 为 `identity` 增加 `features` 与 `supported_node_types`，与同一所选 `aether-profile --capabilities` 文件一致。[读取器特性与身份](reader-features_cn.md)持有精确字段类型、内建 ID、必需特性、旧版回退和负例。IsOS 持有包含支持集合的公共诊断，并在启动前检查实际选中的二进制文件。Setup 适配器保留当前 `ok:false`、`diagnostics:[{path,code,message}]`、`files:[]` 失败封装。当前 setup 代码 `invalid_graph` 和 `unsupported_version` 与 IsOS P05 预检代码分别保留。
