# EOS 节点 profile 与类型化硬件数据

[English](../../../docs/eos/interfaces/hardware.md) · [软件包节点 IDL](node-contributions_cn.md) · [硬件设计](../design/runtime/hardware_cn.md) · [分发层设计](../design/runtime/dispatcher_cn.md)

本文描述 `aether-node-profile` 当前实现的 profile 与 `aether-hal` 类型化硬件数据。[IsOS 图契约](../../island/retired-repository-references_cn.md#ref-5a029c71a447)负责图外层、修订、迁移和连线 QoS；本页负责 EOS 硬件与节点类型准入。Rust 现可读取使用既有类型的 `1.0.0` 图，以及使用已注册 `@v0.1` 类型的 `0.1.0` 图；IsOS 迁移与跨读取器预检仍是独立工作。`Option<T>` 表示值或能力可以缺失，不得伪造；`Vec<T>` 保留顺序。除特别说明外，数值必须有限。

## Profile 结构

profile 是唯一的 `core → layer → group → node` 树。core 是执行与生命周期权威，layer 是领域，group 是节点类型族。每个实例引用已注册的节点类型；类型确定位置、种类、生命周期、允许的能力、设置及类型化端口。读取器接受使用 `@v1.0` 类型的 `1.0.0` 图，以及使用 `@v0.1` 类型的 `0.1.0` 图。硬件地址为 `eos/hardware/<camera|base|manipulator>/<node_id>`。旧八段路径在导入时原子地改写；新图输出短路径。`NodeKind` 由类型确定，不从路径推断。下面的旧版示例包含 EOS 相机、EOS 分发节点和 EGIA 规划节点：

```toml
version = "1.0.0"
[robot]
id = "robot-1"
name = "Robot One"
[cores.eos.layers.hardware.groups.camera.nodes.front]
id = "front"
node_type = "eos.hardware.camera@v1.0"
label = "Front camera"
description = "RGB and registered depth"
frame_id = "front_optical"
capabilities = ["rgb8", "aligned_depth_u16"]
[cores.eos.layers.hardware.groups.camera.nodes.front.adapter]
type = "mock-cxx"
[cores.eos.layers.hardware.groups.camera.nodes.front.health_timing]
poll_interval_ms = 100
stale_after_ms = 500
[cores.eos.layers.hardware.groups.camera.nodes.front.observation]
max_rate_hz = 30.0
max_payload_bytes = 16777216
[cores.eos.layers.dispatcher.groups.control.nodes.command]
id = "command"
node_type = "eos.dispatcher.control@v1.0"
label = "EOS dispatcher"
description = "Admits commands to configured hardware nodes"
capabilities = ["admission", "status"]
references = ["eos/hardware/camera/front"]
[cores.egia.layers.common.groups.process.nodes.guard]
id = "guard"
node_type = "egia.common.guard@v1.0"
label = "EGIA request guard"
description = "Design-only guard for EOS control requests"
capabilities = ["request_guard"]
references = ["eos/dispatcher/control/command"]
```

| 字段 | Rust 类型 | 单位 | 用途与校验 |
| --- | --- | --- | --- |
| `version` | `String` | profile 版本 | 读取器仅接受 `"1.0.0"` 和 `"0.1.0"`；只有 IsOS 写入迁移后的图。 |
| `robot` | `RobotProfile` | — | 指明该节点树所属机器人；必填。 |
| `robot.id` | `String` | — | 稳定机器人标识；必填，只能使用 ASCII 字母、数字、`_` 或 `-`。 |
| `robot.name` | `String` | — | 人类可读名称；非空白。 |
| `cores` | `BTreeMap<String, CoreProfile>` | — | 顶层执行权威，例如 `isos`、`eos`、`egia`；键须是有效地址段，每个 core 至少有一个 layer。 |
| `cores.<core>.layers` | `BTreeMap<String, LayerProfile>` | — | core 拥有的领域；非空，键须是有效地址段。已注册节点类型决定允许的位置。 |
| `...layers.<layer>.groups` | `BTreeMap<String, NodeGroup>` | — | 领域中的节点类型族；非空，键须是有效地址段。已注册节点类型决定允许的位置。 |
| `...groups.<group>.nodes` | `BTreeMap<String, NodeProfile>` | — | 分组内节点；非空，键须是有效地址段并与 `node.id` 相同。完整路径必须唯一；EOS 硬件 ID 还须跨硬件组唯一，供适配器注册。 |
| `node.id` | `String` | — | 稳定地址段；非空且只含 ASCII 字母、数字、`_` 或 `-`，与映射键一致。 |
| `node.node_type` | `String` | 已注册类型 ID | 必填，引用已注册的 `@v1.0` 定义。其 core/layer/group 必须与实例路径一致；未知类型或版本被拒绝。 |
| `node.package` | `Option<PackageRef>` | 软件包 ID 和版本 | 软件包贡献类型必填，且须等于已验证侧文件的 `{id, version}`；内置类型省略。ID 为有效地址段组成的 `owner/name`，版本为三个数字部分。 |
| `node.label` | `String` | — | 简短展示名称；非空白。 |
| `node.description` | `String` | — | 说明节点用途；字段必填，可为空串。 |
| `node.frame_id` | `String` | 坐标系 | 观测和空间命令使用的硬件坐标系；硬件必填且非空白，软件节点应省略。 |
| `node.joint_names` | `Option<Vec<String>>` | — | 静态有序关节权威；manipulator 必填，其他节点禁止。名称非空、唯一且须为有效地址段。观测和关节目标必须符合该顺序。 |
| `node.adapter` | `AdapterProfile` | — | 硬件适配器选择和设置；硬件须提供非空类型，软件保留默认空值。 |
| `node.adapter.type` | `String` | — | 选择 C++ 或 mock 硬件适配器工厂；硬件必填且非空白，软件节点应省略。 |
| `node.adapter.package` | `Option<PackageRef>` | 软件包 ID 与版本 | `mujoco-v2` 等软件包适配器族的精确来源；与固定贡献节点类型的 `node.package` 独立。内置 `mock-cxx` 省略。 |
| `node.adapter.settings` | `BTreeMap<String, toml::Value>` | 适配器自定 | 可选硬件适配器设置，默认为空；适配器负责校验自己的键和值域。软件节点不能设置这些值。 |
| `node.capabilities` | `Vec<String>` | — | 用于发现的能力；软件节点可省略，硬件节点须非空，每项是唯一的有效地址段。camera 必须声明 `rgb8`，可选 `aligned_depth_u16`；base/manipulator 能力与声明的运动模式一致。不表示当前健康。 |
| `node.health_timing` | `HealthTiming` | — | 硬件健康轮询与过期策略；硬件须有有效间隔，软件保留默认零值。 |
| `node.health_timing.poll_interval_ms` | `u64` | ms | 硬件健康检查周期；硬件须大于零，软件节点应省略。 |
| `node.health_timing.stale_after_ms` | `u64` | ms | 硬件健康报告的过期时间；至少等于检查周期，软件节点应省略。 |
| `node.observation` | `Option<ObservationProfile>` | — | 硬件读取边界；camera、base、manipulator 都必填，软件节点禁止。Rust 类型保持可选，以便校验时报告缺少该段。 |
| `node.observation.max_rate_hz` | `f64` | Hz | 最大采样率；有限且大于零。 |
| `node.observation.max_payload_bytes` | `u64` | 字节 | 分配或传送前限制采样大小；大于零。 |
| `node.movement` | `Option<MovementProfile>` | — | 运动模式与限值；base/manipulator 必填，camera 和软件节点禁止。 |
| `node.movement.modes` | `Vec<String>` | — | 非空且唯一；base 为 `body_velocity`、`relative_pose`；manipulator 为 `joint_positions`、`end_effector_pose`。每个模式要求同名能力及相应限值。 |
| `node.movement.limits` | `MovementLimits` | — | 数值上限集合；出现的值都须有限且大于零。 |
| `limits.max_linear_m_s` | `Option<f64>` | m/s | 底盘平移速度上限。 |
| `limits.max_angular_rad_s` | `Option<f64>` | rad/s | 底盘偏航角速度上限。 |
| `limits.max_translation_m` | `Option<f64>` | m | 底盘相对位移上限。 |
| `limits.max_rotation_rad` | `Option<f64>` | rad | 底盘相对偏航上限。 |
| `limits.max_joint_position_rad` | `Option<f64>` | rad | 关节目标位置绝对值上限。 |
| `limits.max_joint_speed_rad_s` | `Option<f64>` | rad/s | 关节目标速度上限。 |
| `limits.max_end_effector_speed_m_s` | `Option<f64>` | m/s | 末端笛卡尔速度上限。 |
| `limits.max_velocity_lease_ms` | `Option<u64>` | ms | 单次底盘速度租约的 profile 上限；声明 `body_velocity` 时必填。准入还要求租约不超过命令剩余期限。 |
| `limits.max_end_effector_translation_m` | `Option<f64>` | m | 末端目标 XYZ 相对其坐标系原点的欧氏范数上限；声明 `end_effector_pose` 时必填。 |
| `node.references` | `Vec<String>` | 规范节点路径 | 指向其他节点的可选定向引用，例如规划节点指向分发节点；默认为空。每条完整路径必须在该 profile 中存在，列表内唯一，且不能指向自身。引用本身不授予硬件所有权或权限。 |
| `node.settings` | `BTreeMap<String, toml::Value>` | 类型特定 | 实例配置，默认空。注册表拒绝未知键、缺失必需键、错误类型和值。IsOS 进程设置见下文。 |
| `node.inputs` | `BTreeMap<String, PortSources>` | 类型化边 | 按接收端口命名的输入连接。来源路径与输出端口必须存在、数据类型相同，且不能自连。底盘与机械臂命令输入基数为 `many`：非空的原语绑定输出数组；每个绑定恰好连接一个设备。迁移期仍接受旧分发器边。其他输入由已注册基数及必填标记决定。 |
| `node.inputs.<port>[i].node` | `String` | 规范节点路径 | 类型化连接的来源实例；必须存在且不能是自身。为兼容旧图，两种基数均可读取对象或数组；新图的多来源编写器输出数组。 |
| `node.inputs.<port>[i].output` | `String` | 输出端口名 | 来源类型的输出端口；必须存在且与接收端口的数据类型一致。 |
| `node.inputs.<port>[i].priority` | `Option<i64>` | nice 刻度 | 可选用户传递优先级；默认 `0`，范围 `-20..19`，仅允许在符合条件的 EOS 本地证据边上。固定或未路由路径显式设置会失败。R01 使用权重 `20 - priority`。 |
| `node.inputs.<port>[i].frequency_hz` | `Option<f64>` | Hz | 可选用户传递限速；缺失表示无连线限速。设置时必须为有限正数，且只能用于符合条件的 EOS 本地证据边；固定或未路由路径拒绝。`Port.expected_rate_hz` 仅是提示。 |
| `node.kind` | `NodeKind` | — | 由已注册类型确定，取值为 `Camera | Base | Manipulator | Software`；不写入 TOML。 |
| `node.path` | `String` | 注册路径 | 校验后生成的 `<core>/<layer>/<group>/<id>` 规范地址，不写入 TOML。 |

软件节点可以声明身份、标签、说明、能力和引用，不得设置非默认的坐标系、关节、适配器、健康时间、观测或运动值；编写 profile 时应省略这些硬件字段。未声明模式所缺失的配置上限并不代表物理运动无限制；适配器与控制器仍需执行物理边界。每个已声明运动模式都必须具备相应限值，分发层在释放命令前检查配置值。

## 节点类型注册表与类型化连接

内置类型 ID 包括 `eos.hardware.camera@v1.0`、`eos.hardware.base@v1.0`、`eos.hardware.manipulator@v1.0`、`eos.dispatcher.control@v1.0`、三种[图控制类型](control_cn.md)、`egia.common` 类型、`isos.process.component@v1.0` 与 `isos.process.component@v1.1`，以及 `eos.process.backend@v1.0`。`Profile::from_toml` 和 `Profile::from_json` 使用内置注册表；`*_with_registry` 接受调用方提供的注册表。`NodeTypeRegistry::all()` 提供可序列化的图编辑器类型目录。注册拒绝重复 ID、无效位置或字段，以及没有兼容且已注册安全适配器的自定义执行器类型。注册不会自行实例化安全适配器；运行时准入必须解析它。

| 注册字段 | Rust 类型 | 单位 | 用途与校验 |
| --- | --- | --- | --- |
| `NodeType.id` | `String` | 类型 ID | 唯一的 `<core>.<layer>.<name>@v<major>.<minor>`；当前内置类型使用 `v1.0`，图校验前注册。 |
| `NodeType.description` | `String`，解码默认 `""` | — | 已实现：新的 `@v0.1` 类型必须有非空用途说明；旧 sidecar 可省略，载入后在内存目录中获得回退说明，不更改已验证字节。内置类型提供说明。 |
| `NodeType.core`、`.layer`、`.group` | `String` | 地址段 | 必需的执行权威、领域和类型族。每项均为有效地址段，实例位置必须匹配。 |
| `NodeType.kind` | `NodeKind` | — | 决定硬件数据与适配器校验。硬件类型必须为 EOS 硬件适配器类型。 |
| `NodeType.lifecycle` | `Lifecycle` | — | `Internal | Process | Adapter`；确定实例启动与停止的责任方。 |
| `NodeType.capabilities` | `Vec<String>` | 能力 ID | 实例可声明能力的上界，每项均为有效地址段。 |
| `NodeType.settings` | `Vec<Setting>` | — | 允许的实例设置；名称不可重复。 |
| `NodeType.inputs`、`.outputs` | `Vec<Port>` | — | 类型化连接面；各方向的端口名不可重复。 |
| `NodeType.safety_adapter_type` | `Option<String>` | 适配器 ID | 注册自定义底盘或机械臂类型时必需；须指向同种类、覆盖其运动模式且支持停止的已注册安全适配器。 |
| `SafetyAdapterType.id` | `String` | 适配器 ID | 自定义执行器引用的唯一有效地址段。 |
| `SafetyAdapterType.kind` | `NodeKind` | — | `Base` 或 `Manipulator`，须与自定义执行器匹配。 |
| `SafetyAdapterType.movement_modes` | `Vec<String>` | 模式 ID | 安全检查覆盖的非空模式集合，须覆盖自定义执行器能力。 |
| `SafetyAdapterType.supports_stop` | `bool` | — | 注册时必须为 true；运行时实现必须执行实际停止。 |
| `Setting.name`、`.data_type`、`.required` | `String`、`SettingType`、`bool` | — | 命名设置模式：`String`、`PositiveInteger`、`NonnegativeInteger`、`StringArray` 或 `StringTable`；必需项不可缺失。 |
| `Port.name`、`.data_type`、`.required` | `String`、`PortDataType`、`bool` | — | 命名输入或输出；必需输入端口必须连接。数据类型为明确的硬件帧、观测、运动、结果、健康、状态或控制请求类型。 |
| `Port.cardinality` | `PortCardinality` | 连接数量 | 默认为 `one`，也可为 `many`；必填的 `many` 输入需要非空数组，且来源互异、类型兼容。 |

### G06 类型化连线准入

IsOS 管理草稿和定位到连线的 Apply 诊断。EOS 在构造路由或适配器前校验自身拥有的已应用连线。为兼容旧图，`node.inputs.<port>` 可使用单个来源对象或来源对象数组。`one` 输入在任一种形式中恰好包含一个来源；`many` 输入至少包含一个互异来源，新图编写器输出数组。`node` 是规范四段路径，`output` 是已注册输出名；可选 `priority` 是 Linux nice 刻度的整数 `-20..19`（默认 `0`）；可选 `frequency_hz` 是以 Hz 为单位的有限正数（缺失表示不限速）。这些字段只描述传递，不能提交执行器命令。旧图缺失的字段不会导致 IsOS 所有的图字节被重写。

EOS 自身拥有的每个来源必须指向存在且非自身的节点，节点位置符合注册的 core/layer/group，来源输出和目标输入拥有完全相同的数据类型 ID，传输声明兼容，输入基数正确且来源互异，并满足所有必填输入。唯一的已注册数据类型描述符管理载荷 schema 及 `x-unit` 注释；注册时拒绝重复 ID。单位一致性由精确类型身份保证，不做转换。旧端口未声明传输时按 `in_process` 解释；新的 `@v0.1` 类型须明确声明传输。EOS 拒绝自身连线形成的环及无效 EOS 引用；IsOS 校验整个跨核心图、EGIA 连线、声明的桥和 IsOS 进程依赖，并在有来源索引时返回它。运行时健康是实时状态，不改变草稿能否编辑或应用。

IsOS 将已准入连线分为 `eos_local`、`egia_local`、`service_bridge` 和 `design_only`；EOS 只使用自身 `eos_local` 投影。只有 EOS 编译了生产者、消费者和传输，本地证据边才可路由；`egia_local` 归 EGIA 所有；Q01 camera-gate 的 `preview` 到 EGIA Vision 证据入口是 IsOS 选定、位于图作者连线之外的只读服务绑定，不是已准入的 `service_bridge`。旧版 `isos.ui.vision@v1.0` preview 不是 `0.1.0` 目标。历史图中通过准入的控制描述边及其他描述性连线保持 `design_only`，直到对应所有者实现传递。IsOS 拒绝没有显式注册服务桥的跨核心连线。规划器提案、软件包贡献类型或通用命令端口都不能绕过 Dispatcher 建立执行器路径。

只有可路由的证据边能设置用户 `priority` 或 `frequency_hz`：EOS 本地的图像、观测、状态、事件边，以及所有者实现路由后的 EGIA 本地证据边。固定命令、原语绑定、安全、停止、结果、健康、就绪和锁存路径，服务桥及仅供设计的连线，均拒绝这两个字段，即使显式优先级是 `0`。底盘或机械臂的 `inputs.command` 来源必须是恰好绑定一个设备的兼容注册原语；Dispatcher 仍独占命令准入与停止。IsOS 报告无效字段或 `/inputs/<port>/<source-index>` 的精确 JSON 指针；EOS 在构造运行时前校验自身投影。R01 再加入实时队列与 `20 - priority` 权重，不改变本准入规则。

### R01 EOS 本地证据连线的实时路由——目标契约

**R01 主题分支已实现。** R01 仅从已验证的 `Profile::eos_edge_contract` 建表。目前唯一可实时传递的精确组合是 `eos.hardware.camera@v0.1.frame` → `eos.interface.camera_gate@v0.1.frame`，数据类型为 `camera_frame`，类别为 `image`，传输为 `in_process`。两个端点须位于当前 EOS 投影中，并有已编译的生产者及消费者。相似类型、软件包贡献端口、旧 `@v1.0` 端口、EGIA 连线和 Vision 服务桥都不能借此获得路由。增加新路线需要先评审 EOS 注册表和 IsOS G06 准入。`VisionService` 每个相机来源只采集一次，并通过此表向声明的 `@v0.1` gate 送帧，不能新增相机采集或执行器通道。

路由键是 EOS 投影内 `(source_node,source_output,target_node,target_input)`，按字节序建表。重复键、缺失的已编译端点或不支持的数据类别须在启动工作线程前失败。`priority` 默认为 `0`，权重精确为 `20 - priority`，所以 `-20` → `40`、`0` → `20`、`19` → `1`。可选 `frequency_hz` 为有限正数；缺失时没有连线级限速。它只限制传递，不请求采样，也不使用端口的 `expected_rate_hz` 提示。限速路线初始有一个令牌，尝试传递时消耗，包括消费者错误。之后须经过正数的单调纳秒：若 `rate <= 1e9`，以 `(elapsed_ns as f64) * rate >= 1e9` 判定；若 `rate > 1e9`，到下一个不同的纳秒刻度才可再有一个令牌。即使经过时间取 `u64` 最大值，上述乘积仍是有限值。不得计算 `1 / rate`、把周期转换为整数时长或积累超过一个令牌。时钟向后观测既不补令牌，也不移动上次消耗时间戳。这样可安全接受最小正次正规数和最大有限 `f64`，避免溢出、零时长忙循环与停顿后突发。IsOS 保持图原始字节及 Apply 所有权。

队列由注册的 `DataClass` 选择：`image` 与 `observation` 每条路线仅保留最新一项，新项替换旧项时增加 `replaced_count` 和 `gap_count`；`state` 与 `event` 使用每路线最多 64 项且序列化载荷最多 1 MiB 的 FIFO。超过任一界限、单项超限时拒绝最新项，保留先前次序，并增加 `overflow_count` 和 `gap_count`。全局最多 256 条实时路线及 64 MiB 排队载荷；路线数量超限在工作线程启动前失败，全局字节压力下拒绝最新候选项并计入溢出和间断，保留已有旧项；最新项队列先按新旧载荷净字节变化检查，再执行替换。当前注册表没有实时 state/event 图路线，相关队列仅用合成调度器测试，不授权新图连线。命令、固定安全／结果／健康／停止和 opaque 数据不入队。消费者错误移除该证据，增加 `consumer_error_count` 和 `gap_count`，不能声称已传递或自动重试。所有路线计数在 `u64::MAX` 饱和，只在新运行时代重置。

现有仅限所有者的运行时 Unix 套接字目标新增 `{"op":"route_status"}`，成功返回 `{"ok":true,"projection_sha256":"<64位小写十六进制>","routes":[...]}`，不包含帧或命令载荷。投影 SHA-256 是 EOS 投影字节摘要，不是 IsOS 保存图修订。只返回源和目标规范路径均以 `eos/` 开头的已验证 **EOS 本地** 声明。旧版已准入的 `egia.common.guard@v1.0` → `eos.dispatcher.control@v1.0` 跨 core `DesignOnly` 契约可能由 `Profile::eos_edge_contract` 返回供校验，但不能成为 EOS 路线状态行；该声明由 IsOS 整图检查器呈现。EOS 本地的仅供设计和固定路线按路由键排序返回。运行时仅在活动期间提供本操作；开始停机时关闭或拒绝，退出后没有已停止快照。无效请求沿用 `{"ok":false,"error":"..."}`，不得泄露载荷。IsOS 状态仍是部署状态权威。

| 响应字段 | JSON 类型及单位 | 用途及校验 |
| --- | --- | --- |
| `ok` | 布尔值 | 完整快照才为 `true`。 |
| `projection_sha256` | 小写十六进制字符串 | 恰好 64 字符，已验证 EOS 投影字节的摘要。 |
| `routes` | 对象数组 | 全部已验证的 EOS 本地声明按键字节序排列；两端均以 `eos/` 开头，其中最多 256 条为实时 R01 路线；无载荷。 |
| `source_node`、`target_node` | 字符串，规范四段 EOS 路径 | 已准入的现有端点，各段通过 profile 路径校验。 |
| `source_output`、`target_input` | 字符串，注册端口名 | 与端点组成唯一键，必须精确匹配注册端口。 |
| `data_type` | 字符串，注册类型 ID | 输入输出共同的精确类型，不推断转换。 |
| `delivery_state` | 枚举字符串 | `active` 实时路由、`not_routed` EOS 本地仅供设计、`fixed_not_scheduled` 固定所有者路线；不提供已停止快照，均非节点健康。 |
| `routed` | 布尔值 | 已准入的 R01 传递路线安装且启用时为 `true`，即使首帧尚未来到；否则为 `false`。 |
| `priority` | 整数或 null，nice 刻度 | 实时路线为 `-20..19`，缺省 `0`；非 R01 为 null。 |
| `frequency_hz` | 有限正数或 null，Hz | 实时路线的可选传递上限；null 表示不限速或非 R01，须结合 `routed` 解读。 |
| `weight` | 整数或 null，调度信用 | 实时路线精确为 `20 - priority`，即 `1..40`。 |
| `queue_class` | `latest`、`fifo` 或 null | 图像／观测选最新，状态／事件选 FIFO；非 R01 为 null。 |
| `queue_depth` | 非负整数或 null，项 | 实时路线保留项数；非 R01 为 null。 |
| `delivered_count` | 非负整数或 null，项 | 本运行时代中消费者已接受的项数。 |
| `cap_wait_count` | 非负整数或 null，访问次数 | 有排队工作但无令牌的调度访问；等待不算丢失。 |
| `replaced_count`、`overflow_count`、`consumer_error_count`、`cancelled_count`、`gap_count` | 非负整数或 null，项 | 分项丢失原因与总间断；非 R01 为 null，计数在 `u64::MAX` 饱和。 |
| `last_delivery_monotonic_ns` | 非负整数或 null，运行时启动后 ns | 最后一次消费者接受时间；尚未接受或非 R01 时为 null，不是墙上时钟。 |

请求及示例 [G06 证据夹具](../../../tests/fixtures/edge_contract/valid_evidence.json)的有效初始快照为：

```json
{"op":"route_status"}
```

```json
{"ok":true,"projection_sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","routes":[{"source_node":"eos/hardware/camera/front","source_output":"frame","target_node":"eos/interface/gate/front","target_input":"frame","data_type":"camera_frame","delivery_state":"active","routed":true,"priority":-20,"frequency_hz":15.0,"weight":40,"queue_class":"latest","queue_depth":0,"delivered_count":0,"cap_wait_count":0,"replaced_count":0,"overflow_count":0,"consumer_error_count":0,"cancelled_count":0,"gap_count":0,"last_delivery_monotonic_ns":null}]}
```

上述摘要只是示例值，不是夹具计算值。无效操作在所有者套接字上返回 `{"ok":false,"error":"unsupported op: bogus"}`。传输失败或缺失路由表是运行时故障，不是成功但空的路线列表。

`delivered_count` 只在消费者接受后增加；`cap_wait_count` 表示无令牌等待，本身不是间断；`queue_depth` 只计保留排队项，不含每路线最多一个正在执行的消费者尝试。停机时 EOS 原子地关闭证据准入并阻止执行中项迟到发布，在清理队列或等待任何证据线程之前调用 Dispatcher 安全停止，之后取消排队项；R01 不能让停滞的消费者或大量队列延迟执行器停止。已就绪的托管 MuJoCo 子进程使用[例行关闭标记](simulator-stop_cn.md#与急停分离的例行关闭状态)；真正的 `safety.estop` 保留持久锁存。独立运行的回退路径使用 `emergency_stop`。每个执行中的消费者有取消令牌和 100 ms 接受期限；取消前接受则计入 `delivered_count`，取消或超期则只计一次 `cancelled_count` 与 `gap_count`，并阻止之后发布。停机取消的每个排队项也各计一次取消及间断。证据线程加入期限为 1 s；超期须报告停止故障，不能声称已完成或无限等待。唯一的 Dispatcher 仍负责执行器安全；R06 负责完整模拟器终止／回收顺序及集成安全停止证据。[R01 夹具](../../../tests/fixtures/routed_edges/README.md)固定实时、拒绝和停止用例。

最终 gate 接受和可见帧槽发布共用调度器准入锁：超期、取消或已封闭的调度器不能发布帧或增加传递计数。最后传递时间戳在接受提交时采样。所有者在 Dispatcher 安全停止前封闭准入，之后才清理排队记录。正次正规数相机 `max_rate_hz` 使用饱和等待时长，不会在就绪前崩溃。

同步相机适配器采集不属于 gate 的 100 ms 接受期限。若停机时仍阻塞，EOS 阻止迟到发布，最多等待证据线程 1 s，随后以非零状态退出且不发出已完成停止事件。Rust 无法杀死阻塞的系统线程；适配器宿主与 R06 模拟器边界分别负责自身终止。

例如 `drive.inputs.command = [{ node = "eos/dispatcher/base/velocity", output = "command" }]` 将原语绑定输出连接至 `BaseMovement` 输入。此边声明图中唯一执行器命令路径，不是运动命令载荷。当前分发层通过相同的已验证规范路径寻址适配器。运行时相机帧和运动命令仍使用下文各自的 Rust 结构体。

IsOS 进程类型使用 `node.settings.command: array<string>`（必需、非空 argv）、`cwd: string`（可选工作目录）、`env: table<string,string>`（可选环境）、`ready_tcp: string`（可选 TCP 就绪地址）、`ready_unix: string`（可选绝对 Unix 套接字就绪路径，少于 108 字节）、`start_timeout_ms: positive integer`（可选毫秒）、`restart: string`（可选 `never | on_failure | always`）、`max_restarts: nonnegative integer`（可选次数）和 `depends_on: array<string>`（可选规范进程节点路径列表；空数组表示无前置进程）。最多设置一个就绪地址。依赖必须互异、存在、为进程类型、不指向自身且无环。IsOS 消费图投影时再校验启动语义。

`isos.process.component@v1.1` 以必需的 `role: string`（EOS 注册表中为 `eos | egia | isosui`）和可选的 `args: array<string>`（各参数非空，允许空数组）取代 `command`。当前 IsOS 进程投影仅启动 `eos` 或 `egia`；IsosUI 由 workbench 服务管理，不是可部署的进程节点。IsOS 为受支持角色解析可执行文件。上文其他进程设置仍可使用。`@v1.0` 可供迁移读取，但 IsOS 不启动其任意命令。

## 共享数据与节点接口

| 结构体.字段 | Rust 类型 | 单位 | 用途与校验 |
| --- | --- | --- | --- |
| `SampleMeta.source_timestamp_ns` | `Option<i64>` | ns | 可选的设备源时间，不能直接用于本地单调期限。 |
| `SampleMeta.clock` | `ClockKind` | — | `Unix | Ros | Device | Unknown`，解释源时间域。 |
| `SampleMeta.received_monotonic_ns` | `u64` | ns | 本地接收时间，由统一 `aether-common::Clock` 提供，用于新鲜度。 |
| `SampleMeta.sequence` | `Option<u64>` | 次数 | 可选源序号，诊断顺序与丢帧。 |
| `SampleMeta.frame_id` | `String` | 坐标系 | 观测所在坐标系；应标明真实源坐标系。 |
| `Health.state` | `HealthState` | — | `Healthy | Degraded | Unavailable`；由适配器观测，不能由配置伪造。 |
| `Health.code` | `Option<String>` | — | 稳定、机器可读诊断码。 |
| `Health.message` | `Option<String>` | — | 有界的人类诊断信息。 |
| `Health.last_ok_monotonic_ns` | `Option<u64>` | ns | 最后一次正常的本地时间，帮助判断过期与恢复。 |
| `NodeStatus.state` | `NodeState` | — | `Starting | Ready | Stopped | Faulted`，节点生命周期。 |
| `NodeStatus.active_command_id` | `Option<String>` | — | 当前执行命令的关联 ID；空闲时缺失。 |
| `CommandMeta.command_id` | `String` | — | 调用关联及避免重复释放；分发时须非空白。 |
| `CommandMeta.deadline_monotonic_ns` | `u64` | ns | 本地单调绝对期限；准入时须晚于当前时间。 |
| `CommandOutcome` | `Accepted | Completed | Cancelled | Ambiguous` | — | 适配器报告；`Ambiguous` 代表物理执行可能已发生，不能自动重试。 |

每个适配器实现 `NodeAdapter` 的 `kind`、`adapter_type`、`status`、`health`、`stop`。注册表在准入前检查 `adapter_type` 与 profile 一致。`CameraNode` 增加 `capture`；`BaseNode`、`ManipulatorNode` 增加 `observe` 与类型化运动方法。只发现节点类型和 profile 实际公布的接口。三个 CXX mock 封装都提供 `from_profile(&NodeProfile, Arc<dyn Clock>)` 构造函数供本地集成测试。

R02 为非锁存停止新增 `NodeAdapter::controlled_stop() -> Result<ControlledStopOutcome, HalError>`，以 `supports_non_latching_stop() -> bool` 明确证明支持能力。默认方法返回不支持，默认能力值为 false。`ControlledStopOutcome` 分为 `Stopped`（物理终态证明）、`ConfirmedFailure(String)`（已知失败）与 `OutcomeUnknown`（释放结果未确认）；Dispatcher journal 将原因限制在 256 UTF-8 字节以内。公开的 `motion.stop` 或 `robot.stop` 同时需要这项类型化证明与 `has_terminal_stop_fence()`。现有 CXX mock 的 `stop()` 会永久锁存，不能仅凭名称作为无锁存公开停止；它仍可用于独立的 `safety.estop` 路径。当前 MuJoCo 适配器在实现受控物理停止之前也保持失败关闭。Dispatcher 在整个操作最多五秒的期限内有界等待每个同步停止部件，在调用前持久化 `Releasing`；超时后每个目标和停止类别仅保留一个执行中的工作线程。Rust 无法杀死超时线程；线程迟到返回不会把原始 `OUTCOME_UNKNOWN` 改写成已确认结果。分离线程结束前继续持有 journal 所有权，防止第二个 runtime 在物理调用未决时接管同一持久 journal。紧急停止使用独立的抢占槽位，阻塞的受控停止不会延迟紧急准入；适配器实现必须支持这两种方法并发调用。

`Dispatcher::dispatch_gateway_stop_checked` 对已持久化准入或语义拒绝的停止返回 `StopDispatchOutcome::Operation(DispatchResult)`。在任何物理释放之前，journal 记录容量不足返回 `PreReleaseCapacity`，对应 gRPC `RESOURCE_EXHAUSTED`；持久写入失败返回 `PreReleaseDurability`，对应 gRPC `INTERNAL`。每种变体均包含供传输无关调用方使用的 `DispatchResult`。未经授权的本地恢复是持久化终态 `REJECTED` 操作，gRPC 仍返回 OK；持久化失败绝不允许物理调用。兼容方法 `dispatch_gateway_stop` 返回所包含的结果。

类型化 journal 操作还为终态准入拒绝保存可选的 `GatewayRejectionCode`。`Unauthorized`、`InvalidState` 与 `Internal` 分别映射为 `rejected.unauthorized`、`rejected.invalid_state` 与 `rejected.internal`；没有此字段的旧记录使用稳定的无效状态回退值。该代码跨重启保留，不依赖供人阅读的 `reason` 文本。成功或仍在等待的操作不含拒绝代码。

持久 journal 启动时把控制状态绑定到已准入 profile 的机器人 ID。已保存的整机目标或控制 ID 属于另一机器人时，在就绪前启动失败；仍处于活动状态却缺少机器人目标身份的旧控制记录也失败关闭。非活动的旧控制记录可以绑定到已准入机器人，并持久保存该 ID。这样其他机器人的 profile 不能继承其停止或急停状态。

运动准入等待写者优先的停止释放门时，只等待命令剩余的单调时钟期限，最长五秒。排队工作线程过期后返回持久化拒绝，停止门打开也不能再释放运动。已取消的 `Prepared` 排队命令保持 `CANCELLED`，不会迟到调用适配器。

### 软件包适配器共享库 ABI

已验证软件包可以为新的相机、底盘和机械臂适配器族提供 Linux 共享库。软件包清单声明 `adapters: [{family, library, sha256}]`：`family` 是贡献的适配器族 ID，`library` 是软件包内相对路径，`sha256` 是 64 位十六进制 SHA-256 摘要。IsOS 验证软件包，并以 `--adapter-library <family>=<绝对路径>@<sha256>` 将精确路径交给 EOS。EOS 对已打开文件的 inode 重新计算摘要并加载该 inode，检查 `aether_adapter_v1` 描述符中的 ABI 版本、适配器族和设备类型，再为每个节点建立唯一实例。实例销毁前共享库保持加载。MuJoCo 继续使用独立进程和套接字；`mock-cxx` 仍作为内置 mock。

部署时 EOS 为每个软件包硬件节点启动独立适配器宿主进程；只有宿主映射软件包共享库。父进程保留 dispatcher 准入权，通过仅所有者可访问的 Unix 套接字发送版本 `1` 的有界 JSON 请求：`heartbeat`、`call`（C ABI 操作码与类型化载荷）及 `stop`。响应为 `{ok,result}` 或 `{ok:false,error}`；相机响应上限为 64 MiB，其余为 1 MiB。宿主连续 500 ms 未收到心跳时，即使其他调用被阻塞也调用适配器的独立 `stop`，随后退出。父进程把宿主丢失或 IPC 超时视为不可用，绝不自动重试运动命令。若宿主本身死亡，实体控制器仍须自行执行租约。此 IPC 用于控制和观测，不传输每次 100 Hz 总线周期；驱动在宿主内以最新获准目标运行本地总线循环。

| 宿主 IPC 字段 | 类型 | 用途与校验 |
| --- | --- | --- |
| `version` | `u32` | 必填请求模式版本；仅接受 `1`。 |
| `op` | `"heartbeat" \| "call" \| "stop"` | 必填操作；不进行隐式回退。 |
| `operation` | `u32` | `call` 必填；C ABI 操作码 `1–7`。 |
| `request` | 类型化 JSON 值 | `call` 必填；匹配 C ABI 操作载荷。 |
| `ok` | `bool` | 必填响应成功标记。 |
| `result` | 类型化 JSON 值 | 成功时存在；匹配 C ABI 结果。 |
| `error` | `String` | 失败时存在；人类可读的拒绝原因。 |

公开 C 头文件为 [`include/aether/adapter_v1.h`](../../../include/aether/adapter_v1.h)。ABI 版本是整数 `1`；`family` 是以 NUL 结尾的 UTF-8 ID；`kind_mask` 使用相机 `1`、底盘 `2`、机械臂 `4`；`create` 接收 `{path, kind, node}` JSON（规范路径、派生类型、已验证节点 profile）并返回由共享库拥有的实例；`call` 的操作码 `1–7` 对应状态、健康、相机采集、底盘观测/运动、机械臂观测/运动；`stop` 必须能够与被阻塞的 `call` 并发，并在返回前锁存停止状态；`destroy` 释放实例。输入输出缓冲区由调用方拥有，相机采集输出上限为 64 MiB，其余输出上限为 1 MiB。`call` 返回非零状态时输出 UTF-8 错误。类型化 JSON 载荷遵循下方结构；运动请求接收 `{meta, movement}`，返回 `accepted`、`completed`、`cancelled` 或 `ambiguous`。实体适配器或控制器负责独立、有界的停止和运动租约。

## 相机 RGB 与配准深度

| 结构体.字段 | Rust 类型 | 单位 | 用途与校验 |
| --- | --- | --- | --- |
| `CameraFrame.meta` | `SampleMeta` | — | RGB 与深度共用的来源、坐标系、新鲜度。 |
| `CameraFrame.width_px` | `u32` | px | 图像宽度；大于零。 |
| `CameraFrame.height_px` | `u32` | px | 图像高度；大于零。 |
| `CameraFrame.stride_bytes` | `u32` | 字节/行 | RGB 行跨度；至少为宽度的三倍。 |
| `CameraFrame.rgb8` | `Vec<u8>` | 字节 | 交错 RGB8；长度等于行跨度乘高度。 |
| `CameraFrame.depth` | `Option<DepthFrame>` | — | 同帧配准深度；不支持时明确缺失。 |
| `CameraFrame.preview_jpeg` | `Option<CameraJpeg>` | — | 同一 RGB 图像的可选 JPEG 预览；无 JPEG 源时缺失。 |
| `CameraJpeg.bytes` | `Vec<u8>` | 字节 | JPEG 预览，4 字节至 8 MiB，须有 JPEG 起止标记；计入相机载荷上限。 |
| `CameraJpeg.quality` | `u8` | JPEG 质量 | 源编码质量，1–100；宽度和质量匹配时 Vision 可复用原始字节。 |
| `DepthFrame.depth_u16` | `Vec<u16>` | 缩放深度值 | 每个 RGB 像素一个值，长度等于宽乘高。 |
| `DepthFrame.depth_scale_m` | `f64` | m/值 | 原始深度到米的比例；有限且大于零。 |
| `DepthFrame.intrinsics` | `Intrinsics` | — | 对齐深度使用的相机标定。 |
| `Intrinsics.fx` | `f64` | px | 横向焦距；有限且大于零。 |
| `Intrinsics.fy` | `f64` | px | 纵向焦距；有限且大于零。 |
| `Intrinsics.cx` | `f64` | px | 主点横坐标；有限。 |
| `Intrinsics.cy` | `f64` | px | 主点纵坐标；有限。 |
| `Intrinsics.distortion_model` | `String` | — | 标定模型名称；非空。 |
| `Intrinsics.distortion_coefficients` | `Vec<f64>` | 模型自定 | 按顺序排列的畸变系数；各项有限。 |

`CameraFrame::validate(max_payload_bytes)` 检查尺寸、缓冲区长度、配准、标定与 RGB、深度和预览的总字节上限。

## 底盘观测与运动

| 结构体.字段 | Rust 类型 | 单位 | 用途与校验 |
| --- | --- | --- | --- |
| `BaseObservation.meta` | `SampleMeta` | — | 实测状态的来源与新鲜度。 |
| `BaseObservation.valid` | `bool` | — | 位姿与速度是否可用；为假时禁止依赖状态的命令。 |
| `BaseObservation.x_m` | `f64` | m | `meta.frame_id` 下实测 x；有效时须有限。 |
| `BaseObservation.y_m` | `f64` | m | 实测 y；有效时须有限。 |
| `BaseObservation.yaw_rad` | `f64` | rad | 实测偏航；有效时须有限。 |
| `BaseObservation.x_m_s` | `f64` | m/s | 实测 x 速度；有效时须有限。 |
| `BaseObservation.y_m_s` | `f64` | m/s | 实测 y 速度；有效时须有限。 |
| `BaseObservation.yaw_rad_s` | `f64` | rad/s | 实测偏航角速度；有效时须有限。 |
| `BaseMovement::BodyVelocity.x_m_s` | `f64` | m/s | 本体坐标系 x 速度；有限且在平移限值内。 |
| `BaseMovement::BodyVelocity.y_m_s` | `f64` | m/s | 本体坐标系 y 速度；有限且在平移限值内。 |
| `BaseMovement::BodyVelocity.yaw_rad_s` | `f64` | rad/s | 本体坐标系偏航角速度；有限且在角速度限值内。 |
| `BaseMovement::BodyVelocity.valid_for_ms` | `u64` | ms | 生产控制器到期必须停止的短租约；大于零，不超过 `max_velocity_lease_ms`，且处于命令剩余期限内。CXX mock 到期后的读取报告零速度，但没有物理 watchdog。 |
| `BaseMovement::RelativePose.x_m` | `f64` | m | 控制器接收的相对 x 位移；有限且在位移限值内。 |
| `BaseMovement::RelativePose.y_m` | `f64` | m | 相对 y 位移；有限且在位移限值内。 |
| `BaseMovement::RelativePose.yaw_rad` | `f64` | rad | 相对偏航；有限且在旋转限值内。 |
| `BaseMovement::RelativePose.frame_id` | `String` | 坐标系 | 相对目标坐标系；必须等于 `node.frame_id`，不做隐式坐标变换。 |

`body_velocity` 和 `relative_pose` 是两个独立声明的模式。相对目标交由控制器执行；HAL 不规划路线。

## 机械臂观测与运动

| 结构体.字段 | Rust 类型 | 单位 | 用途与校验 |
| --- | --- | --- | --- |
| `ManipulatorObservation.meta` | `SampleMeta` | — | 关节和工具状态的来源与新鲜度。 |
| `ManipulatorObservation.joint_names` | `Vec<String>` | — | 有序关节名称；非空、唯一。 |
| `ManipulatorObservation.joint_positions_rad` | `Vec<f64>` | rad | 与名称同序的完整实测位置；长度相同，有效时有限。 |
| `ManipulatorObservation.joint_velocities_rad_s` | `Vec<f64>` | rad/s | 与名称同序的实测速度；长度相同，有效时有限。 |
| `ManipulatorObservation.joints_valid` | `bool` | — | 关节数组能否用于状态依赖的准入。 |
| `ManipulatorObservation.end_effector_pose` | `Option<EndEffectorPose>` | — | 可选的实测工具位姿。 |
| `ManipulatorObservation.end_effector_valid` | `bool` | — | 工具位姿是否可用。 |
| `EndEffectorPose.position_m` | `[f64; 3]` | m | XYZ 工具位置；每项有限。 |
| `EndEffectorPose.orientation_xyzw` | `[f64; 4]` | 单位四元数 | XYZW 朝向；有限且模长约等于一。 |
| `EndEffectorPose.frame_id` | `String` | 坐标系 | 工具位姿坐标系；非空白。 |
| `ManipulatorMovement::JointPositions.joint_names` | `Vec<String>` | — | 有序目标名称；必须与适配器完整关节集合一致。 |
| `ManipulatorMovement::JointPositions.joint_positions_rad` | `Vec<f64>` | rad | 与名称同序的完整目标数组；长度相同、有限且在限值内。 |
| `ManipulatorMovement::JointPositions.max_speed_rad_s` | `Option<f64>` | rad/s | 可选调用方速度上限；有限、大于零且不超配置限值。 |
| `ManipulatorMovement::EndEffectorPose.pose` | `EndEffectorPose` | m 与四元数 | 交由控制器处理的笛卡尔目标；`frame_id` 必须等于 `node.frame_id`，不做隐式变换或 Rust IK。 |
| `ManipulatorMovement::EndEffectorPose.max_speed_m_s` | `Option<f64>` | m/s | 可选调用方笛卡尔速度上限；有限、大于零且不超配置限值。 |

分发层拒绝 profile 未声明的模式。C++ 适配器或设备控制器负责运动学、轨迹、碰撞保护与物理限位。

## 分发请求与发现

以下类型位于 `aether-dispatcher`。命令准入在进程内完成；`Dispatcher::new_durable` 跨重启保存命令身份和结果，`Dispatcher::new` 的内存日志供测试使用。

| 结构体.字段 | Rust 类型 | 单位 | 用途与校验 |
| --- | --- | --- | --- |
| `CommandRequest.node_path` | `String` | 规范节点路径 | 以 profile 中的完整路径选择一个已准入硬件节点；未知或软件节点路径会被拒绝。本地节点 ID 只用于适配器注册。 |
| `CommandRequest.meta` | `CommandMeta` | — | 上文所述关联 ID 与期限。 |
| `CommandRequest.movement` | `Movement` | — | `Base(BaseMovement) | Manipulator(ManipulatorMovement)`，须匹配节点类型。 |
| `DispatchResult.command_id` | `String` | — | 回显请求关联 ID，供查询结果。 |
| `DispatchResult.status` | `OutcomeStatus` | — | `Completed | Cancelled | Rejected | InProgress | OutcomeUnknown`；物理结果不确定时保留未知终态。 |
| `DispatchResult.reason` | `Option<String>` | — | 拒绝或不确定时的人类诊断；确认成功时缺失。 |
| `NodeDescriptor.path` | `String` | 注册路径 | 供硬件发现与命令使用的完整 `<core>/<layer>/<group>/<id>` 规范地址。 |
| `NodeDescriptor.node_id` | `String` | — | 注册适配器时与 profile 匹配的稳定本地 ID。 |
| `NodeDescriptor.kind` | `NodeKind` | — | 已准入硬件节点的相机、底盘或机械臂分类。 |
| `NodeDescriptor.label` | `String` | — | profile 中的人类可读名称。 |
| `NodeDescriptor.facets` | `Vec<NodeFacet>` | — | 可用的 `Status | Health | Observation | Movement | Safety` 接口。 |
| `NodeDescriptor.capabilities` | `Vec<String>` | — | 声明的能力，与实时健康区分。 |

`Dispatcher::new(&Profile, Vec<AdapterRegistration>, Arc<dyn Clock>)` 要求每个硬件节点恰好对应一个节点类型和适配器类型都匹配的注册。`discover()` 返回已准入硬件节点描述；`capture_camera(node_path)` 返回经校验且新鲜的相机帧；`dispatch(CommandRequest)` 返回结果；`result(command_id)` 查询所选日志；`stop_node(node_path)`、`cancel`、`emergency_stop` 进入安全接口。`aether-runtime` 的 `Runtime::from_file` 最多读取 1 MiB profile，完成校验与分发器组合。该运行时是进程内基础代码，尚非已部署的 `eosd` 入口。
## 注册表验证

单元与桩图测试覆盖已知和未知节点类型、版本与位置拒绝、类型化端口不匹配、JSON/TOML 往返、IsOS 进程设置与依赖环，以及自定义执行器安全注册。中国时间 2026-10-30，`cargo test -p aether-node-profile -p aether-dispatcher` 通过 8 个 profile 和 25 个分发层测试，`cargo test -p aether-runtime --lib` 通过 4 个运行时测试，`cargo clippy -p aether-node-profile -p aether-dispatcher --all-targets -- -D warnings` 通过。完整本地输出见 `logs/test/20261029T180750Z/node-profile.log`。


### EOS 包后端与 EGIA common 类型

`eos.process.backend@v1.0` 位于 `eos/process/backend`。它描述由 EOS 管理的包进程，不包含直接的 `command`；可执行文件和场景由 IsOS 从已验证的包中解析，EOS 负责启动、监测和停止子进程。

| 设置 | 类型 | 单位 | 用途与校验 |
| --- | --- | --- | --- |
| `package` | `String` | 包 ID | 必填，格式为有效地址段组成的 `owner/name`，例如 `island/mujoco-sim`。 |
| `version` | `String` | 包版本 | 支持的 `island/mujoco-sim` 后端必填 `0.1.1`，用于锁定已解析的包。 |
| `scene` | `String` | 场景 ID | 必填且非空；IsOS 对照包中的场景和资源检查。 |
| `socket` | `String` | Unix 套接字路径 | 必填的绝对路径，长度小于 108 字节；IsOS 校验运行目录，EOS 管理套接字服务子进程。 |
| `env` | `StringTable` | 环境变量 | 可选字符串表；IsOS 在启动时校验。 |
| `start_timeout_ms` | 正整数 | ms | 可选启动就绪期限，须大于零；EOS 将其限制在 120,000 ms 以内。 |

四种仅供设计的 EGIA 类型是 `egia.common.source@v1.0`（`egia/common/data`，输出 `observation: ObservationRef`）、`egia.common.context@v1.0`（输入 `ObservationRef`、输出 `context: ContextSnapshot`）、`egia.common.planner@v1.0`（`egia/common/process`，输入 `ContextSnapshot`、输出 `proposal: PlanProposal`）及 `egia.common.guard@v1.0`（输入 `PlanProposal`、输出 `request: ControlRequest`）。这些是配置和类型化连接定义，尚不表示运行中的 EGIA。`egia.tensor` 保留给未来统一张量空间；当前图不创建空 layer。

| 端口类型 | 用途 | 校验 |
| --- | --- | --- |
| `ObservationRef` | 引用选定观察源而不复制硬件载荷。 | 端口类型必须完全一致；未来载荷结构独立于节点配置。 |
| `ContextSnapshot` | 传输有界的推理上下文。 | 端口类型完全一致。 |
| `PlanProposal` | 将提议动作送交 guard。 | 端口类型完全一致。 |
| `ControlRequest` | 经过 guard 后请求 EOS dispatcher 准入。 | 端口类型完全一致；dispatcher 仍负责执行器安全。 |

## 0.x 目录实现部分

新编写的图使用 `version = "0.1.0"`，`node.node_type` 选择 `@v0.1` 注册类型。现有 `node.description` 仍解释具体实例；新实例需要非空文字，导入的旧实例可在编辑前保留原来的空值。注册类型另有非空 `description`，说明功能及操作员可见行为。注册表为旧图保留 `@v1.0` 定义；旧 sidecar 缺少新字段时提供安全回退描述。保存的图中类型 ID 不会被静默重写。

EOS 注册新的内置身份 `eos.hardware.camera@v0.1`、`eos.hardware.base@v0.1`、`eos.hardware.manipulator@v0.1`、`eos.dispatcher.control@v0.1`、`eos.dispatcher.primitive_base@v0.1`、`eos.dispatcher.primitive_manipulator@v0.1` 和 `eos.interface.camera_gate@v0.1`。它们沿用各自类型族的位置、种类、生命周期、能力及类型化端口名；目录发布完整描述和设置元数据。新图中的 dispatcher 控制节点表示唯一的共享准入方，包括 IsOS 初始图；原语绑定仍通过该方受控的路径连接硬件。旧 `eos.control.manual@v1.0`、`eos.control.primitive@v1.0`、`eos.control.skill@v1.0` 和 `eos.process.backend@v1.0` 不增加 `@v0.1` 别名。MuJoCo 软件包贡献 `eos.process.mujoco@v0.1`；EGIA 和 IsOS 各自拥有其新类型描述符。IsOS 汇总目录时，EOS 不重新定义这些描述符。

目标注册表在不改变现有 `name`、`data_type` 与 `required` 含义的前提下扩展每个 `Setting`：

| 字段 | 类型和单位 | 用途、默认值、校验与兼容性 |
| --- | --- | --- |
| `description` | 可选非空字符串，— | 设置的操作提示；新的 `@v0.1` 定义必填，旧定义缺省时使用设置名称作为回退。 |
| `unit` | 可选字符串，声明的物理单位 | 用于显示与文档；缺省表示没有物理单位，不表示自动转换。 |
| `min`、`max` | 可选有符号 64 位整数，设置单位 | 整数设置的含边界范围；值须非负且 `min <= max`。解析数据类型后作精确比较。没有边界的旧设置保持原校验。 |
| `choices` | 可选、不重复的字符串数组，— | 静态允许值；空或缺省表示不作静态限制。升级后不可用的当前值仍可见，但 Apply 拒绝。 |
| `selector` | 可选对象，— | 动态选项来源，见[节点贡献](node-contributions_cn.md#目标设置选择器)；缺省使用普通编辑器输入，不能授权未验证的软件包路径。 |

边界仅适用于整数类型设置，必须是整数、非负且处于声明类型的范围内；静态选项仅适用于字符串设置。设置不得同时声明静态限制和动态选择器。注册时校验选择器的 provider、resource 及包来源；IsOS 在 Apply 前根据当前已验证选项校验选中的值。新的内置类型包含端口说明和适用的传输/数据类别元数据；只有所属方声明了实测频率，才设置 `expected_rate_hz`。目录元数据不改变精确端口数据类型身份，也不授权命令。

EOS 读取器先按图版本解析，再载入精确的软件包 sidecar，按对应版本规则校验类型及实例描述，并在构造适配器前核对全部端口与数据类型。新增字段在解码时可选，因此旧 sidecar 和 profile 仍可读取。缺少 `0.1.0` 支持的旧程序在启动前被 IsOS 预检拒绝；未知未来版本不猜测。图迁移不改变已发布的 gRPC 字段号、ProtoJSON 名称、HTTP 路由、适配器 ABI 或 MuJoCo 套接字版本。

**G05 设置读取契约。** `aether-profile setup --json` 的身份保留 `graph_version:"1.0.0"`、`node_type_version:"1.0"` 作为旧标量别名，增加 `supported_graph_versions:["1.0.0","0.1.0"]` 与 `supported_node_type_versions:["1.0","0.1"]`。后者只描述 EOS 拥有的类型标识，不表示可接受任意具有这些后缀的 ID。其 `0.1.0` Validate/Render 路径从完整的 IsOS 图中投影 `eos` core，使用精确的 EOS 内置类型与已验证的软件包 sidecar 校验该 core，并输出按修订保存的 `eos/graph.json` 运行时投影。对于 `0.1.0`，IsOS 使用这个已验证的生成路径启动 `aether-runtime` 与 `aether-config-server`，不会将保存的整图路径交给它们。迁移和 Run 前，IsOS 校验自己的进程 core 与全部跨 core 连线、引用；G06 准入尚不可用时拒绝所有跨 core 连线和引用。EGIA 校验自己的 agent core。`1.0.0` 路径保留当前整图行为以兼容旧图。未知 EOS 类型和错误软件包来源会在构造适配器前失败。EOS 不会虚构 IsOS 或 EGIA 类型描述符。运行时、profile 工具和配置服务也在能力列表中声明 `graph-0.1-reader`。[P05 读取器特性契约](reader-features_cn.md)新增在迁移、Apply、Run 或 Restart 前检查实际所选二进制文件与必需特性；此跨仓库预检仍属目标工作。

G04 测试夹具序列化全部七种新的 EOS 内置类型及 MuJoCo 进程 sidecar，检查每个类型的位置、种类、生命周期、能力、设置、端口身份和元数据，并覆盖既有目录路由。拒绝用例包括重复 ID、空描述、无效边界或选项、错误选择器、未知编解码器及错误的软件包来源。兼容用例确认全部旧内置类型与缺少新元数据的 `@v1.0` sidecar 仍可读取，且其字节不变。图用例覆盖含 dispatcher 的新初始图、固定软件包版本的通用 MuJoCo 设备、每种执行器各一个原语绑定；错误路由、目标、位置和包引用在构造适配器前失败。

G04 验证：`cargo fmt --all -- --check`、`cargo clippy -p aether-node-profile -p aether-runtime --all-targets -- -D warnings` 和 `cargo test -p aether-node-profile -p aether-runtime` 于中国时间 2026-11-01 通过。完整本地输出位于 `logs/test/20261101-g04-node-catalog/`。实时 MuJoCo 测试需要运行中的仿真器，因此聚焦命令跳过了它。

## R03 Dispatcher 准入目标契约

**R03 经合并后的 EOS 与 IsOS 所有者验收。** [gRPC 所有者契约](grpc_cn.md#r03-执行器受治理准入目标契约)规定外部修订、授权载体与错误。本节规定进程内 Dispatcher 边界。受管理执行器命令带不可变 `AdmissionContext`：`active_revision: String` 为 IsOS 保存整图 SHA-256，`authority_role: EgiaControl | OperatorControl` 为已验证角色而非调用方自述，`ingress: Grpc | PrivateBridge` 为服务端证明的通道而非调用方自述，`binding_path: String` 为规范注册的 primitive 节点路径，`primitive: String` 与 `primitive_version: String` 为精确已准入目录身份。`CommandRequest.meta.command_id` 仍是调用方 1–128 字节 ID，`deadline_monotonic_ns` 仍是本机单调绝对期限。`node_path` 仍是由 binding 派生的精确硬件目标，不能作为客户端另选的目的地。受管理运动必须提供这些字段；旧进程内 `dispatch` 调用只用于本地兼容/测试，不能成为生产入口。图连线声明 binding，不是命令。

唯一的 Dispatcher 准入入口在排队后的最终释放闸门，将不可变上下文与活动图代际比较。调用 adapter 前拒绝缺失或变化的保存修订、未知角色、缺失或多目标 binding、错误 primitive/mode 或 adapter kind、不健康或过期健康、无效或过期必需观测、非有限数值、配置界限或 frame/单位不匹配、过期截止时间、紧急锁存及运动 inhibit。健康和观测新鲜度使用 `Clock::monotonic_ns()` 与 profile 的 `stale_after_ms`，不用来源壁钟。采样失败使该节点降级，直到新鲜有效探测恢复；不能授予另一条路径。停止操作有自己的持久类型。两个受管理角色都可在缺少当前修订时请求 `safety.estop`，但必须通过认证；内部关闭使用私有安全接口。证据路由及其上限不能准入或静默丢弃运动/停止。

持久记录的规范身份包括命令 ID、IsOS 部署运行 ID、稳定角色/范围与入口、保存图活动修订、primitive/版本、binding 路径、精确目标及类型化参数。凭据字节、传输 request ID 和单次尝试期限不进入签名；新尝试仍须在释放前满足实时期限。journal 在物理释放前同步 Prepared 与 Releasing，保存终态或未知结果。同 ID 相同身份返回保留结果；同 ID 不同身份拒绝且不释放。恢复的 Releasing 命令为 `OutcomeUnknown`，在物理证明的显式恢复前抑制运动，不自动重试。释放前写入失败不调用 adapter；可能释放后的故障不能宣称成功。缺少新身份的历史 journal 记录不能按新图重新解释。[R03 夹具](../../../tests/fixtures/r03_admission_revision/README_cn.md)固定假时钟、持久重启与绕过测试。
