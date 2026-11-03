# Aether EOS 契约

[English](../../docs/island/contracts_aether.md) · [EOS 契约架构](../eos/interfaces/contracts_cn.md) · [约定](conventions.v1.3.0_cn.md)

**范围：**本页列出已发布的契约源码和可选 C++ 服务接口。0.x 目标为计划中的 Rust gRPC 网关保留兼容的 Gateway 和 Capability 身份，但**不**新增 WorkflowService、ActionList 或图内技能序列。现有 `workflow.proto` 与相关 protobuf 名称仍是历史兼容身份。请通过 [EOS 活动接口目录](../eos/interfaces/README_cn.md)和[历史目录](../eos/legacy/README_cn.md)区分目标行为与保留的源码。

**Island** 是团队，**Aether** 是产品版本，**EOS** 拥有这些公开接口。手工维护的源码位于 `share/eos/contracts/eos/`；简短的路径不决定传输包名。`island.eos.v1` 继续用于 Gateway、Skill 与 Workflow；`CapabilityService` 使用 `island.eos.v2`。Schema ID 继续使用历史名称空间 `egia.contracts.*`；最新载荷现统一使用 v1 ID。

## 源码、产物与所有权

Gateway、Skill、Workflow、system、robot、perception 与 primitive 的公开接口只在 `share/eos/contracts/eos/` 下的小型 `.proto` 文件中维护。四个服务入口 `gateway.proto`、`skills.proto`、`workflow.proto` 和 `capabilities.proto` 位于 EOS 契约根目录。对应的请求与响应消息放在领域文件夹中；例如 `skills.proto` 定义 RPC，`skills/execution.proto` 定义执行载荷。`system/` 存放资源与事实消息，`robot/` 存放描述、导航与 primitive binding，`perception/` 存放相机与感知载荷，`options/` 存放共用 JSON Schema annotation，`external/http/` 存放 HTTP 路由注解。`actions/`、`body/` 和 `world/` 定义生成的运行时 JSON Schema 家族。具体文件见[源码分布](../eos/interfaces/contracts_cn.md#源码分布)。Vendored Google API annotation 位于 `share/eos/contracts/google/`。独立且可缺省的载荷使用 optional 字段，互斥载荷使用 `oneof`。私有模型提供方 schema 和 gRPC 服务定义位于 `share/eos/contracts/eos/external/models/`；模拟器协议位于 `external/simulator/`。原生 southbound HAL 没有公开 JSON Schema。

生成工具编译 proto descriptor，在 `share/eos/schemas/eos/` 下生成运行时 JSON Schema 与 `contracts.manifest.json`：`actions/` 存放 primitive 调用与确认，`body/` 存放机体校验载荷，`world/` 存放感知与状态资源。生成的 JSON 可以审查，但不能作为手工编辑的源码。可选 north HTTP 网关从 proto annotation 生成 `share/ext/north-http/openapi/north-http/openapi.json`，包括 HTTP 专用及流式路由。`EOS_NORTH_HTTP` 控制网关及 OpenAPI 文件的构建和安装；`north-http.toml` 保存监听配置。CMake 在构建目录生成 C++ binding。Island-egia 在自己的仓库维护特定语言的镜像。

## 公开边界

可选的 C++ EOS 服务通过原生 gRPC 提供 `GatewayService`、`SkillService`、`WorkflowService` 和 `CapabilityService`。默认 Rust 进程尚未提供此网关。EGIA 初始化时检查传输健康，并获取有效的 `Discover` 和 `ListResources` 响应。`Discover.devices[].primitives` 是 primitive 发现接口。每个精确的 `(primitive, version, device_id)` binding 提供类别、允许的 reference 模式、有界数值范围、健康状态、依赖、媒体类型及可选的绝对坐标系。智能体从 `ListCapabilities` 选择对应规范名称；EOS 在硬件执行前校验解析后的绑定。Northbound 没有设备或 primitive 注册 API；紧凑的机器人与设备状态是 `Discover` 和 `GetStatus` 的类型化字段。

Action 请求的参数由选定的生成 Schema 约束：

```json
{"capabilityName":"hardware/demo/base/default/base.velocity@1.0.0","operationId":"move-42","arguments":{"linear_x_m_s":0.3,"linear_y_m_s":0.0,"angular_z_rad_s":0.1,"valid_for_ms":250}}
```

EOS 0.2.0 退役公开的 v1 Gateway primitive 动作 RPC。`CapabilityService.SubmitOperation` 接收规范 `capabilityName`、`operationId` 与 object `arguments`。选定的 action schema 拒绝未知字段；EOS 在校验前注入内部的 `type` 和 `command_id`。JSON Schema 默认值只是注解，不会自动填充。数值按语义使用米、弧度、牛顿及毫秒。精确载荷与发现规则见 [primitive 接口](../eos/interfaces/actions-payloads_cn.md)。

终态操作包含结果状态非空的 `primitive.ack`。Command ID 在执行前预留；相同重试重放原响应，冲突复用不会执行。稳定的拒绝通过成功 RPC 中的 `accepted: false` 表示；畸形输入与未知 primitive identity 使用传输错误。设备能力解析到已准入设备；全机 stop 能力使用 `runtime/{robot_id}/...` 名称空间。

已发布的 C++ `SkillService` 提供可搜索 descriptor 与异步执行，包括执行快照、截止时间、重放、信号、日志和取消。技能参数必须通过返回的 Schema 校验后才能执行。安装由运维操作完成，客户端不能注册可执行路径。North HTTP 将异步 API 映射为 REST 与 SSE。见 [技能编写](../eos/interfaces/skills/native/README_cn.md)。

EOS 定义 external world、arm state、raw RGB vision、semantic detection、compact VAD 与 compact ASR 的 discriminator 载荷。External-world Schema 当前没有可读路由。Detection 不包含 mask、keypoint 或 tensor；紧凑 VAD 与 ASR 不包含音频及提供方数据。Raw RGB 通过受限的 `CaptureFrames` 快照接口读取。资源载荷按生成的 Schema 校验；未知 ID 不是兼容别名。

## 修改契约

保持仍在使用的 RPC 名称、字段编号、ProtoJSON 名称与公开 discriminator 兼容。此前 EOS 0.2.0 切换移除了五个公开 v1 Gateway primitive 动作 RPC，也改变部分精确 Schema/primitive ID；其特定客户端迁移见[迁移指南](../eos/interfaces/capability-migration_cn.md)。以后接口和节点配置扩展遵循 [0.x 兼容规则](conventions.v1.3.0_cn.md#统一可观测性与发布记录)：新增带默认值的可选字段，尽量保持旧读取器支持的传输形态，并让新读取器接受旧数据。不兼容变更应使用新契约标识与明确迁移，不静默替换 ID。在仓库根目录重新生成并检查产物：

```sh
uv sync --only-group contracts
python3 scripts/generate_eos_contracts.py --output share/eos/schemas/eos
python3 scripts/generate_eos_contracts.py --output share/eos/schemas/eos --check
python3 scripts/generate_north_http_openapi.py --output share/ext/north-http/openapi/north-http/openapi.json
python3 scripts/generate_north_http_openapi.py --check
./scripts/print_contract_interfaces.py --contracts "$PWD/share"
python3 tests/eos/test_contract_manifest.py "$PWD/share"
python3 tests/eos/test_contract_interfaces.py "$PWD/share"
```

检查 descriptor 和 ProtoJSON 兼容性、Schema 接受范围、路由覆盖、C++ 消费端与两种 HTTP 构建设置。每次公开变更同步更新 [EOS 接口指南](../eos/interfaces/contracts_cn.md) 和相关测试。部署专用技能与可变状态保留在本地，并由 Git 忽略。
