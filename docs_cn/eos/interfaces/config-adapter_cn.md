# EOS 配置适配器

`eos-config` 是 IsOS 使用的 EOS 配置接口。它不启动 `eosd`、不监听端口、
不访问硬件，也不写入部署文件。原生程序安装在 `libexec/island/eos-config`；
公开的 `bin/eos-config` 包装器支持源码、便携版和系统安装布局。

运行 `eos-config describe|defaults|profiles|render|validate --json`。
每次响应都是包含 `schema_version: 1`、`ok`、`data` 和 `diagnostics` 的
JSON 对象。诊断包含 `path`、`code`、`message`；请求失败时退出码为 1。
`render` 和 `validate` 从标准输入读取一个 JSON 对象：

```json
{"schema_version":1,"values":{"grpc_uds_path":"/run/user/1000/isos/eos.sock"}}
```

`defaults` 提供可编辑字段：`host`、`grpc_port`、`grpc_uds_path`、
`grpc_uds_mode`（十进制权限位）、`north_http_port`、`binding_backend` 和
`binding_profile`。`render` 返回 `eos/eos.toml` 与
`ext/north-http/config.toml` 的相对路径和内容，由 IsOS 写入托管配置目录。
生成的 EOS TOML 使用运行时同一解析器校验。每个 EOS 进程必须恰好有一个
mode binding。
默认 binding 为相对于生成的 `eos/eos.toml` 的
`bindings/isos-demo.toml`。使用此默认值时，`render` 还返回
`eos/bindings/isos-demo.toml`，因此生成的目录无需引用 EOS 源码树。
无技能包或 simulator 时 EOS 仍可启动并提供发现接口；只有单独运行
simulator IPC 服务，并用 `EOS_SIMULATOR_SOCKET` 指向其 socket 后，
demo 设备操作才会就绪。

`profiles` 提供 `eos.native`（一个 EOS 进程及 TCP gRPC 输出）和
`eos.uds`（同一进程上的逻辑 gRPC 端点）。只有父 EOS 进程绑定 socket 后，
UDS 节点才算就绪。两者声明真实的 Gateway v1、Skill v1、Capability v2、
Workflow v1 服务。IsOS 必须创建进程所属且权限为 `0700` 的私有 socket
父目录，等待就绪后才能启动 UDS 使用者；适配器不会创建该目录。

每个 profile 的有序 `fields` 数组供图编辑器使用。字段包含 `name`、
`type`、`label`、`description`，以及适用的 `default`、`required`、
`min`、`max` 或 `options` 约束。`eos.native` 描述监听、binding 和
socket 设置；由 IsOS 管理父进程和端点时，`grpc_uds_path` 来自端点。
`eos.uds` 提供必填图设置 `socket_path`，其 UDS 就绪字段也使用该名称。
`render` 和 `validate` 接受 `socket_path` 作为 `grpc_uds_path` 的别名；
两者同时出现但值不同时拒绝请求。原有的 `grpc_uds_path` 输入仍受支持。

托管默认端口为 loopback gRPC `16200` 和 north HTTP `16201`。
`grpc_uds_path` 默认为空，由 IsOS 提供每个部署独有的私有路径。
两个监听端口必须为 `1..65535` 的整数，且不能相同。UDS mode 是
`1..511` 的十进制整数，EOS 将其渲染为八进制 TOML 值。
