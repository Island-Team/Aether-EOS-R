# EOS 契约架构

[English](../../../../docs/eos/develop/interfaces/contracts.md) · [接口概览](README_cn.md) · [Aether 契约规则](../../../island/contracts_aether_cn.md)

EOS 公开契约的源码位于 `share/contracts/eos/`。根目录中的 `gateway.proto`、`skills.proto` 和 `workflow.proto` 是三个服务入口，定义供其他进程使用的 gRPC 服务和 HTTP 路由注解。各领域文件夹存放对应消息。无论源码路径如何变化，传输包名保持 `island.eos.v1`。

## 源码分布

| `share/contracts/eos/` 下的位置 | 内容 |
| --- | --- |
| `gateway.proto`、`skills.proto`、`workflow.proto` | 服务 RPC 与 HTTP binding |
| `system/` | 资源与事实 |
| `robot/` | 机器人描述、导航与 primitive binding |
| `perception/` | 相机、媒体、人脸、人体、姿态、角色、VAD 与监控 |
| `skills/` | Catalog、执行、日志、信号及共用消息 |
| `workflow/` | Action、状态及模板消息 |
| `options/` | 共用 JSON Schema annotation |
| `actions/`、`body/`、`world/` | 生成运行时 JSON Schema 的 proto 源码 |
| `external/http/` | HTTP 适配器的路由注解 |
| `external/models/` | 私有模型提供方 gRPC 契约与能力 JSON Schema |
| `external/simulator/` | 私有模拟器 gRPC socket 契约 |

例如，修改 SkillService RPC 时编辑 `skills.proto`，修改执行载荷时编辑 `skills/execution.proto`。文件夹只用于组织源码，不改变 protobuf 包名或 Schema ID 名称空间。

Proto option 表达 protobuf 类型本身无法描述的 JSON 校验规则。生成工具在 `share/ext/eos/` 下生成运行时 JSON Schema 和清单：

- `actions/`：15 个 primitive 调用与 `primitive.ack`。
- `body/`：7 个设备类别的校验载荷。
- `world/`：9 个感知与状态资源载荷。

生成的清单记录角色、路径、Schema ID 和摘要；EOS 启动时加载并校验。Schema ID 仍使用历史名称空间 `egia.contracts.*`，每个现行载荷统一使用一个 v1 ID。`Discover.devices[]` 和状态字段由 protobuf 定义；body-device schema 校验 EOS 的投影数据，不额外创建可读取资源。

使用 `EOS_NORTH_HTTP` 启用外部 north HTTP 包时，同一组 proto 定义生成 `share/ext/north_http/openapi.json`。适配器将 REST/SSE 和 WebSocket 路由转换为原生 gRPC 调用。监听配置仍在 `north-http.toml` 中。

primitive 调用的外层 envelope 由 protobuf 定义，所选生成的 action schema 校验 `arguments`：

```json
{"name":"base.velocity","version":"1.0.0","deviceId":"base-0","commandId":"move-42","arguments":{"linear_x_m_s":0.3,"linear_y_m_s":0.0,"angular_z_rad_s":0.1,"valid_for_ms":250}}
```

行为说明见 [body](body_cn.md)、[world](world_cn.md)、[primitive 调用](actions_cn.md) 和 [HTTP/SSE](external/http_cn.md)。模型提供方的私有 request/response schema 位于 `share/contracts/eos/external/models/schemas/`；`body/*.proto` 定义公开本体载荷的 schema，不新增可读取资源。原生硬件 HAL 没有共享 JSON Schema。

## 更新契约

修改专注的 proto 模块，重新生成 JSON，并审查生成的 Schema 和 OpenAPI 变化。保持 RPC 名称、protobuf 字段编号、ProtoJSON 字段名和公开 discriminator 的兼容性。本次仅保留最新形态的 v1 重置替换了原 v2 Schema ID 与 primitive 版本，客户端必须与 EOS 同步更新精确标识和接受的载荷。

```sh
uv sync --only-group contracts
python3 scripts/generate_eos_contracts.py --output share/ext/eos
python3 scripts/generate_eos_contracts.py --output share/ext/eos --check
python3 scripts/generate_north_http_openapi.py --output share/ext/north_http/openapi.json
python3 scripts/generate_north_http_openapi.py --check
./scripts/print_contract_interfaces.py --contracts /absolute/path/to/share
```

运行契约、Schema 与网关测试。HTTP 路由变更时也使用 `EOS_NORTH_HTTP` 构建。模型提供方的私有载荷不参与此公开生成流程。
