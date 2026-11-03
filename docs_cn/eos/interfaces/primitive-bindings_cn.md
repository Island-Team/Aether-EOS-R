# EOS v2 原语绑定与类型化命令信封

[English](../../../docs/eos/interfaces/primitive-bindings.md) · [硬件 profile](hardware_cn.md) · [图控制](control_cn.md)

EOS 原语绑定命名一个带版本的运动操作，并把它连接到恰好一个硬件设备。IsOS 图拥有绑定边；EOS 读取该边，校验类型化命令头，并通过共享 Dispatcher 传递解码后的 Rust 运动值。绑定本身不拥有驱动，也不授权直接释放执行器命令。

## 图契约

内置类型 `eos.dispatcher.primitive_base@v1.0` 和 `eos.dispatcher.primitive_manipulator@v1.0` 分别位于 `eos/dispatcher/base/<id>` 与 `eos/dispatcher/manipulator/<id>`。其 `command` 输出类型分别为 `base_movement` 与 `manipulator_movement`。通用硬件类型 `eos.hardware.base@v1.0` 和 `eos.hardware.manipulator@v1.0` 的 `inputs.command` 基数为 `many`，即一个或多个 `{node,output}` 的显式数组。每个绑定必须恰好连接到一个兼容设备。绑定没有目标设置；图边就是目标。此路径隐式使用共享 Dispatcher 服务，因此图不必包含 `eos.dispatcher.control@v1.0`。

```json
{"id":"velocity","node_type":"eos.dispatcher.primitive_base@v1.0","label":"Base velocity","description":"Bounded velocity binding","settings":{"primitive_name":"base.velocity","primitive_version":"1.0.0","movement_type":"body_velocity"}}
```

```json
{"command":[{"node":"eos/dispatcher/base/velocity","output":"command"}]}
```

| 字段 | 类型 | 单位 | 用途与校验 |
| --- | --- | --- | --- |
| `settings.primitive_name` | 字符串 | 原语 ID | 必填；当前底盘为 `base.velocity`，机械臂为 `arm.movej`。 |
| `settings.primitive_version` | 字符串 | 语义版本 | 必填；当前两个已准入原语均为 `1.0.0`。 |
| `settings.movement_type` | 字符串 | 模式 ID | 必填；`base.velocity` 对应 `body_velocity`，`arm.movej` 对应 `joint_positions`；设备必须声明此模式。 |
| `inputs.command` | `PortSource` 数组 | 类型化图边 | 执行器必填；来源各不相同、类型兼容，且为绑定输出。一个绑定恰好对应一个设备。 |
| `PortSource.node` | 字符串 | 规范路径 | 来源实例 `<core>/<layer>/<group>/<node>`；必须存在。 |
| `PortSource.output` | 字符串 | 输出端口 | 必须是来源已注册的 `command` 输出。 |
| `node.adapter.type` | 字符串 | 适配器族 ID | 选择一个设备所有的已编译工厂，如 `mujoco-v2` 或 `mock-cxx`。 |
| `node.adapter.package` | `{id:string,version:string}` 或缺省 | 软件包引用 | 软件包适配器族必填；精确匹配已验证软件包，与内置节点类型的 `node.package` 独立。 |
| `node.adapter.settings` | 对象 | 适配器族专用 | 经校验的设备端点设置；MuJoCo 使用 `backend`、`socket_path`、`simulator_node`、`device_id`。 |

注册目录的 `primitive_specs` 包含 `name`、`version`、`device_class`、`movement_type`、`description`。`adapter_families` 包含 `id`、`device_kinds`、`transport`、`description`、`supported_primitives`、可选 `settings_schema`、可选精确 `package`，以及由 EOS 计算的 `compiled` 和 `reason`。软件包侧文件可以声明适配器族字段，但不能声明代码已编译。`GET /api/eos/v2/primitive-specs`、`GET /api/eos/v2/adapter-families` 与 `aether-profile catalog --node-types <已验证侧文件>` 提供这些目录。已编译的 `mujoco-v2` 适配器族支持 `island/mujoco-sim@0.1.1` 的相机、底盘及机械臂设备。一个适配器拥有一个物理设备及其类型化方法和停止方法；原语不创建第二个适配器。

计划中的 IsosUI EOS 面板对**活动图中的每个绑定实例显示一项控制**，不按原语名称合并。它从活动图解析该绑定的唯一目标，显示类型描述与实例描述，并可按原语目录及硬件限值生成类型化参数表单。草稿绑定、相同显示名或用户自填目标不得产生另一条路径。[层数据流演示](../../island/layer-dataflow_cn.md#首个原语控制演示目标-ui-设计)定义操作顺序和反馈状态。

## 有界运行时命令

已合并的 EOS R03 实现中仅所有者可访问的 Unix 控制 socket 接收最多 64 KiB 的单行 JSON。新绑定使用 `primitive_typed` 操作；`payload` 是[硬件 v2](hardware_cn.md)定义的类型化运动对象。**私有** `payload_length_bytes` 统计此对象经解析后通过 `serde_json` 紧凑 UTF-8 序列化的字节数。回环 HTTP bridge 接收有界的公开紧凑 UTF-8 长度，在解析后规范化私有请求头，因为 JavaScript 和 Rust 对相同有限数字的序列化字节数可能不同；见[bridge 契约](control_cn.md#回环-api)。解码器在请求 Dispatcher 准入前拒绝未知字段、超过 16 KiB 的载荷、不支持的编解码器或版本、伪造的图目标，以及原语与载荷类型不匹配。Dispatcher 继续校验健康、截止时间、限值、日志和停止。

已合并的 EOS R03 实现中，浏览器调用经现有回环 `/api/eos/v2/control` bridge：它允许 `primitive_typed`，检查 Origin 和大小后转发到此私有 runtime 操作。浏览器先通过 bridge 取得 EOS 单调时钟时间，不得用自身墙上时间生成 `deadline_monotonic_ns`。EOS 再次验证类型化头、精确活动绑定及目标。响应中的 `command_id`、精确的 `Completed`/`Cancelled`/`Rejected`/`InProgress`/`OutcomeUnknown` 状态和原因驱动面板结果显示；浏览器本地的发送状态仅表示等待。浏览器 bridge 将有界公开 `result` 改写为受保护的私有 `primitive_result`；[R07 授权结果契约](control_cn.md#r07-浏览器授权结果后续查询)定义 UI 后续查询 `InProgress` 所需的有界且按权限校验的读取。不确定结果绝不显示为已完成。这是现有契约的 UI 入口，不是新图连线或 gRPC 服务名。

```json
{"op":"primitive_typed","header":{"data_type_id":"base_movement","data_type_version":"1.0","primitive_name":"base.velocity","primitive_version":"1.0.0","source_path":"eos/dispatcher/base/velocity","target_path":"eos/hardware/base/drive","command_id":"move-1","sequence":1,"deadline_monotonic_ns":5000000000,"payload_length_bytes":83,"codec":"json_v1"},"payload":{"type":"body_velocity","x_m_s":0.1,"y_m_s":0.0,"yaw_rad_s":0.0,"valid_for_ms":100}}
```

以上示例记录历史 R02 请求形状；已合并的 EOS R03 实现因其缺少活动修订而在运动前拒绝。[R03 受管理准入](grpc_cn.md#r03-执行器受治理准入目标契约)要求 `header.active_revision` 等于 IsOS 保存整图活动修订；受监管 bridge 在私有 IPC 外层自行加入 `authority_grant`，浏览器不能发送或读取该授权。缺失/过期修订或错误授权在 adapter 调用前拒绝；已合并的 EOS bridge 已注入私有操作员授权；R07 仍负责按权限校验的结果后续查询及完整浏览器路径验收。直接私有 IPC 的 `payload_length_bytes` 必须等于发送端紧凑 `serde_json` 字节；公开 HTTP 中浏览器发送 1–16,384 字节的有界提示，bridge 按自身解析载荷的序列化长度改写私有字段，随后由 runtime 校验。历史示例值仅作展示。成功返回 `{"ok":true,"result":<DispatchResult>}`。不合法的命令头返回 `{"ok":false,"error":"target_path differs from the graph binding edge"}` 或其他有界校验错误。命令头出错时不会调用适配器。

| 命令头字段 | 类型 | 单位 | 用途与校验 |
| --- | --- | --- | --- |
| `data_type_id` | 字符串 | 注册 ID | `base_movement` 或 `manipulator_movement`，须与解码后的载荷及绑定端口匹配。 |
| `data_type_version` | 字符串 | IDL 版本 | 必须为 `1.0`。 |
| `primitive_name` | 字符串 | 原语 ID | 须与绑定的 `primitive_name` 相同。 |
| `primitive_version` | 字符串 | 语义版本 | 须与绑定的 `primitive_version` 相同。 |
| `source_path` | 字符串 | 规范图路径 | 须以短路径精确指向绑定节点。 |
| `target_path` | 字符串 | 规范图路径 | 须与图边选定的唯一硬件设备相同。 |
| `active_revision` | 字符串 | 保存字节 SHA-256 | 受管理运动必填 64 位小写十六进制，等于 EOS 已准入 IsOS 活动修订；历史 R02 示例缺失该字段，现于 adapter 释放前拒绝。 |
| `command_id` | 字符串 | 命令身份 | 非空、至多 128 个 UTF-8 字节；Dispatcher 还校验 ID 格式及重复策略。 |
| `sequence` | 无符号整数 | 命令序号 | 必须为正数；调用方提供，用于排序及追踪。 |
| `deadline_monotonic_ns` | 无符号整数 | ns | EOS 单调时钟截止时间；Dispatcher 拒绝过期命令。 |
| `payload_length_bytes` | 无符号整数 | 字节 | 私有 IPC：精确的紧凑 `serde_json` 载荷长度，范围 1 至 16,384；公开 HTTP：有界提示，由 bridge 在私有 IPC 前规范化。 |
| `codec` | 字符串 | 编解码器 ID | 必须为 `json_v1`；EOS 在 CXX 方法之前解码成 `Movement`。 |

## 兼容与通道

图导入接受旧的 `cores/<core>/layers/<layer>/groups/<group>/nodes/<node>` 形式，并原子地把全部引用及边改为短路径。混用两种路径形式的图校验失败。旧的单对象 `inputs.command` 在导入时改为数组。旧 `primitive` 操作与 `eos.control.primitive@v1.0` 仍可解析既有图。已合并的 EOS R03 实现已在部署的私有 socket 上拒绝旧操作；新绑定节点使用受治理的 `primitive_typed`。历史进程内兼容 helper 不构成部署入口。相机高频路径使用现有有界最新值类型化 hub 和专用只读 Vision WebSocket 流。高频执行器通道留待后续实现；当前命令路径是低频复用的 Unix 控制通道。
