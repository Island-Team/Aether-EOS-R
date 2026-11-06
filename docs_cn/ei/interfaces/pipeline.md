# EGIA 阶段接口，目标 0.1.0

[English](../../../docs/ei/interfaces/pipeline.md) · [架构](../design/architecture.md) · [当前 IsOS 适配器](isos-config.md)

## 边界与状态

本文是**目标契约**，尚未成为已实现的 wire 协议。阶段接口是 Python 进程内的有类型边界，投影到 IsOS 图中以便配置和观察。IsOS 拥有图的序列化、source edge 对象、节点类型目录和带修订检查的编辑；见其[节点图契约](../../isos/interfaces/README_cn.md)。EOS 拥有 gRPC 和物理 payload；EGIA 拥有以下阶段值与算法。A01 的 EGIA reader 现已验证七种精确阶段类型和每类一个实例的完整图，渲染绑定修订的 `stages.json`，并在 managed 启动时把各实例绑定到现有 Python core 所有者。带 stub 的 EOS 证据沿有类型的 link → perception → context → decision 路径传递。Mission/Plan 状态以及固定的安全与紧急出口仍由当前模块拥有。IsOS 目前仍只准入历史 EGIA 设计类型，因此新节点的 IsOS Apply/Run 还需独立的 IsOS reader/catalog 变更；A04 已为两条精确的所有者证据路径加入调度。IsOS G06 在独立的路径注册前仍拒绝 EGIA 边 QoS；result/health 反馈也不属于此 A04 切片。

新图使用 `0.1.0`，新类型 ID 使用 `@v0.1`。每个注册类型必须有非空 `description`，配置实例也保留自己的 `description`。Reader 仅按既有安全行为接受旧 `1.0.0` 图和精确的历史 `egia.common.source@v1.0`、`egia.common.context@v1.0`、`egia.common.planner@v1.0` 与 `egia.common.guard@v1.0` 设计身份；旧设计类型不会因此得到执行路径。新的 `@v0.1` 阶段是新身份，不是这四种类型的自动别名或迁移结果。未知未来版本或未知类型会在验证时给出路径和支持集合。新增可选字段、端口可保持兼容；改变端口含义、单位、方向或已有类型 ID 必须发布新类型版本。阶段目录不会改动现有 protobuf 字段号或 EOS service 名称。

托管 `egia/stages.json` 覆盖文件携带 `revision: string`，恰好为 IsOS 保存的完整图 SHA-256 的 64 个小写十六进制字符。它是所选部署身份，不是 EOS 投影摘要，也不是图中编写的设置。EGIA 在 Link 就绪前拒绝缺失或格式错误的值；包括强制刷新在内的每个完整发现代际都要求 EOS `eos-active-revision` 与之相等。即使 EOS 各响应内部一致，只要属于另一张保存图，就不能发布就绪阶段或准入 primitive。

## 公共阶段值

阶段端口在 Python 内传递有类型对象。下列 JSON 是日志或 inspector 使用的**调试投影**，不是取代 EOS protobuf 或 EGIA WebSocket 的新传输协议：

```json
{
  "schema_version": "0.1.0",
  "event_id": "01JAZQ3D5Q4N8PK1Y3V6W0M2BS",
  "run_id": "run-20261101-01",
  "source_node": "egia/evidence/perception/main",
  "backend": "robot-a",
  "observed_at": "2026-11-01T08:00:00Z",
  "kind": "observation",
  "payload": {"resource_ref": "camera/front", "status": "available"}
}
```

| 字段 | 类型 / 单位 | 用途与默认值 | 验证、所有者与兼容性 |
|---|---|---|---|
| `schema_version` | 语义版本字符串 / 无 | 调试投影 schema；必填。 | EGIA；支持 `0.1.0`，可增加可选字段；reader 无法安全解释的未来版本必须拒绝。 |
| `event_id` | 非空不透明字符串 / 无 | 单个事件 ID；必填。 | EGIA 生产者；一次运行内唯一，同一事件重试时保持不变。 |
| `run_id` | 非空不透明字符串 / 无 | 关联一次进程启动；必填。 | EGIA supervisor；所有阶段继承，新进程不能复用。 |
| `source_node` | 规范 `core/layer/group/node` 路径 / 无 | 无歧义地标记来源；必填。 | EGIA 投影；必须对应声明类型的节点；只有叶子 ID 无效。 |
| `backend` | 稳定 backend alias 或 null / 无 | 标识源 EOS；纯操作员事件默认为 null。 | EGIA link；非 null 时必须存在于渲染后的 mode profile；不能用 EOS 进程 UUID 代替。 |
| `observed_at` | RFC 3339 UTC 时间 / 秒 | 观测的源时间；观测事件必填，其他事件为创建时间。 | EGIA 来源阶段；必须可解析；另外保存接收时间做过期检查。 |
| `kind` | 枚举 / 无 | `discovery`、`observation`、`context`、`mission`、`proposal`、`admission`、`rejection`、`result`、`health`。 | EGIA；未知值拒绝，不能当作通用命令。 |
| `payload` | 按 kind 区分的有类型值 / 无 | 阶段数据；必填。 | EGIA 所属阶段；转发前验证；兼容 reader 可忽略新增的可选字段，未知动作变体默认拒绝。 |

所有图可配置阶段设置使用 IsOS setting descriptor，包含名称、类型、标签、描述、默认值、必填标记、验证规则和可选 selector 元数据。新注册类型的 `description` 必须非空；旧 sidecar 在读取时可得到仅用于展示的兼容描述。图边 `priority` 与 `frequency_hz` 是 IsOS 的传递控制，不改变 Mission 优先级或安全准入。

## 阶段目录与端口

所有类型的注册 `core` 都是 `egia`。端口名在 `@v0.1` 内稳定；`?` 表示可选输入。端口是进程内有类型流或快照，不是网络监听。`expected_rate_hz` 是生产者提示；边的 `frequency_hz` 是 IsOS 图契约规定的传递上限。非空类型描述必须解释阶段用途，并与实例描述一同显示在 IsosUI。

A04 所有者调度器只为两条精确的进程内证据路径编译边 QoS：`egia.link.gateway@v0.1.resource → egia.perception.admission@v0.1.resource` 和 `egia.perception.admission@v0.1.observations → egia.context.builder@v0.1.observations`。可选 `priority` 是 `-20..19` 的整数，默认 `0`，公平调度权重为 `20 - priority`；较小的 nice 值获得更多服务，但其他就绪边不会饥饿。可选 `frequency_hz` 必须是有限正数；缺省表示不限速。每条边只保留一个最新待传值，替换时增加公开的 `gaps` 和 `overflow` 计数并记录 `evidence_overflow`；传递采用单调时钟，停止时清空待传证据。`stage_edge_delivery` 所有者日志给出计数。`context.snapshot → decision.snapshot` 以及 discovery、result、health、registry、mission 状态、proposal、latch、admission 和紧急停止路径都不接受此控制，继续使用不封顶或固定的所有者行为。IsOS G06 在独立的精确路径目录变更前仍拒绝 EGIA 本地边 QoS；此所有者实现本身不使 IsOS 图生效。

所属方目录为每个注册端口给出 `data_type: egia.stage.<kind>@v0.1`、`transport: in_process` 和精确的 `data_class`。输入还声明 `cardinality: one` 与 `required`；进程所属入口和固定命令出口带 `fixed: true`，不能作为用户编写的图边。`catalog.data_types` 为每种端口值提供一个对象 JSON Schema，列出有类型的必需字段，并在固定单位字段标注单位。IsOS 可以将这些 `stage_types` 汇入图目录，但这些 schema 仍仅由 EGIA 定义。目录 schema 描述结构，运行中的值仍须在所属阶段转发前验证。

A01 本地运行时拒绝来自其他 run 的事件，也拒绝接收时 `observed_at` 已超过 300 秒的事件；未来或无效时间戳同样被拒绝。此初始界限用于实时阶段传递，且在重复事件记录之前检查。Resource 准入边界要求 `sample.text` 与 `sample.eos_robot_id` 均为非空字符串，之后才保留或转发事件。拒绝诊断包含稳定代码、源节点和事件 ID；校验失败后，修正的事件可复用该 ID。

| 类型 ID / layer / group | 输入端口 | 输出端口 | 类型描述与生命周期 |
|---|---|---|---|
| `egia.link.gateway@v0.1` / `io` / `link` | `gateway`（已验证 EOS 端点）、`admitted`（受控内部请求）、`emergency_stop`（仅 monitor 的内部请求） | `discovery`（完整代际）、`resource`（EOS 证据）、`result`（操作结果）、`health` | 把稳定 backend alias 连接到 EOS；先于下游启动，发现降级时阻止动作，关闭时释放通道。 |
| `egia.perception.admission@v0.1` / `evidence` / `perception` | `resource`、`hearing?`、`result` | `observations`（有界事件/快照视图） | 验证并限制带来源的证据；错误证据记录诊断并丢弃，不能变成空成功。 |
| `egia.context.builder@v0.1` / `cognition` / `context` | `observations`、`body?`、`memory?` | `snapshot`（有界模型上下文） | 在不修改源状态的前提下确定性渲染上下文；缺少可选输入时省略对应区块。 |
| `egia.missions.queue@v0.1` / `cognition` / `missions` | `intent`、`result?` | `selected`、`lifecycle` | 准入和调度带版本的 Mission；保存 checkpoint，队列空时等待。 |
| `egia.decision.planner@v0.1` / `cognition` / `decision` | `selected`、`snapshot` | `proposal`、`plan_state` | 通过已配置模型 lane 提出有类型步骤；模型失败或错误输出只产生失败提案。 |
| `egia.safety.gate@v0.1` / `governance` / `safety` | `proposal`、`registry`、`latch` | `admission`、`rejection` | 只准入精确匹配的 primitive；到 link 的命令出口是固定运行时路径，普通图边不能接入。 |
| `egia.monitor.health@v0.1` / `governance` / `monitor` | `health`、`progress`、`bounds?` | `status`、`latch` | 监控健康与进度；紧急出口仅使用固定运行时路径。 |

| 值或设置 | 类型 / 单位 | 用途与默认值 | 验证、所有者与兼容性 |
|---|---|---|---|
| `gateway` | 端点引用 / 无 | IsOS 解析的 EOS gRPC TCP/UDS 端点；managed 模式必填。 | IsOS 解析；EGIA 验证传输类型和 UDS 绝对路径；HTTP 端口无效。旧 TCP profile 省略 `transport` 时仍按 TCP 解释。 |
| `resource` | EOS 有类型资源引用与样本 / 源单位 | 证据；无默认值。 | EOS 拥有 payload；link 验证来源和 schema；未知引用不能变成能力。 |
| `discovery` | 完整的 EOS 资源/能力发现代际 / 无 | Link 就绪及精确绑定；完整代际完成前不存在。 | Link 原子发布；部分或冲突的发现默认拒绝；底层 protobuf 字段由 EOS 拥有。 |
| `hearing` | 带来源的操作员话语 / 文本及 UTC 时间 | 可选证据；未配置输入源时不存在。 | EGIA 北向接口检查身份与大小；perception 拒绝错误或缺来源输入。 |
| `body` | 已验证的 EOS body 快照 / 源单位 | 可选硬件上下文；发现前不存在。 | EOS 拥有物理描述；link 验证 robot 和资源身份后才交给 context。 |
| `memory` | 有界召回记录 / 文本及分数 | 可选先前上下文；未启用或不可用时不存在。 | EGIA memory 管理保留和来源；context 限制数量及 token，不能把召回当作源真值。 |
| `observations` | 有界序列/快照 / 源单位 | 提供给 context 的证据；有效但尚无证据时为空。 | Perception 管理准入、排序、合并、省略计数；新观测变体需要准入策略。 |
| `snapshot` | 有界上下文文档 / token | 供模型读取的状态；允许空的可选区块。 | Context 管理总 token 预算和确定性排序；不含原始秘密或无限媒体。 |
| `intent` / `selected` | Mission 文档与修订 / 无 | 操作员请求与已选工作；队列空时不存在。 | Missions 管理身份、来源、优先级、版本和 checkpoint；拒绝过期修订。 |
| `lifecycle` | Mission ID、修订和状态转移 / 无 | 传递 Mission 变化；发生转移前不存在。 | Missions 管理单调修订和持久 checkpoint；同 ID 重复转移幂等，冲突失败。 |
| `proposal` | 有类型的规划 primitive 引用 / 无 | 候选动作；无默认值。 | Decision 解析模型输出；必须引用 backend、device、primitive、version 和参数；没有执行权。 |
| `plan_state` | Plan ID、修订、阶段和结果 / 无 | 报告规划进度；选中 Mission 前不存在。 | Decision 管理；无效转移和过期修订失败，模型超时不算成功。 |
| `registry` | 完整、已验证的能力代际 / 无 | 安全门查询；发现成功前不存在。 | Link/registry 管理精确 backend/device/primitive/version 绑定；safety 拒绝缺失或过期代际。 |
| `admission` / `rejection` | 安全门决定及原因 / 无 | 放行或解释拒绝；每个提案只有一个终结决定。 | Safety 管理；放行数据必须精确绑定当前 registry；未知、过期、无效数据默认拒绝。 |
| `result` | EOS 操作 ID 与终结状态 / 无 | 反馈；无默认值。 | EOS 管理终结操作语义，link 验证身份；超时和取消不是成功。 |
| `health` / `latch` | 状态枚举和单调 episode / 无 | 报告降级及紧急状态；只有完成初始化验证后才能视为健康且未锁存。 | Monitor 管理；锁存状态只能经授权重置，不能靠重启清除。 |
| `progress` | Mission/操作 ID 和单调已用时间 / 秒 | Monitor 输入；没有运行工作时不存在。 | Decision/link 管理测量值；monitor 限制过期，不能从无更新推断完成。 |
| `bounds` | 有限测量值和配置界限 / 源单位 | 可选 monitor 安全输入；未配置界限来源时不存在。 | Monitor 检查单位和来源身份；缺少必需界限时默认拒绝。 |
| `status` | 健康状态、episode、原因 / 无 | 面向操作员的监控投影；初始化为不可用。 | Monitor 管理状态转移；过期或部分 backend 状态不能映射为健康。 |
| `admitted` / `emergency_stop` | 有类型的命令引用 / EOS 单位 | 固定运行时出口，不是用户可编辑图线；无默认值。 | Safety 管理工作负载准入，只有 monitor 管理紧急停止；link 验证调用者，EOS 负责最终准入。 |
| `model_binding` | 非空 selector ID / 无 | 为 decision lane 选择已配置模型端点；启用 lane 时必填。 | EGIA 验证其存在于 `models.toml`；选项来自 IsOS 包/配置目录。图不保存原始权重路径；新增 binding 不会自动准入新模型动作。 |
| `backend_alias` | 唯一字符串 / 无 | 到 EOS 的稳定路由；link 实例必填。 | EGIA 验证唯一、非空；进程 UUID 仅用于诊断。 |

`@v0.1` 阶段目录仅开放下表列出的图可编辑设置；其他 Python 运行时设置仍属于已安装默认文件和[配置指南](../lib.md)中的进程配置。首版 `context` 与 `safety` 没有图可编辑设置：context 预算来自已验证的进程配置，safety 策略仍位于 `safety/gate.toml`，不能任意通过节点修改。EGIA 拥有类型；IsOS 验证保存值和 render 请求，EGIA 启动时再次验证。省略可选设置时使用安装默认值；已有设置名与含义保持不变。新增设置应添加有默认值的可选字段，否则发布新类型 ID。

| 阶段 / 设置 | 类型 / 单位 | 用途与安装默认值 | 验证与兼容性 |
|---|---|---|---|
| `link.backend_alias` | 字符串 / 无 | 稳定 EOS 路由；每个 link 必填。 | 非空，link 实例之间唯一；旧单 backend 模式规范化为 `default`。 |
| `link.gateway` | 端点引用 / 无 | IsOS 解析的 EOS gRPC 端点；managed 模式必填。 | TCP host/port 或绝对 UDS 路径；拒绝 HTTP control/catalog 端点；旧 TCP profile 没有 `transport` 时仍解释为 TCP。 |
| `perception.capacity` | 整数 / 记录数 | 准入 FIFO 上限；64。 | `1..10000`；溢出计数并报告省略。 |
| `perception.history_per_kind` | 整数 / 每类记录数 | Prompt 历史深度；8。 | `1..10000`；修改它不重新解释已保存事件。 |
| `missions.mission_capacity` | 整数 / 待处理 Mission 数 | 队列准入上限；8。 | `1..1024`；队列满时拒绝新 Mission。 |
| `decision.controller.model` | selector ID / 无 | 快速模型 binding；`gemma4-fast`。 | 非空、存在于 `models.toml`、不同于 reasoner；不可用的已保存 ID 仍显示但阻止 Apply。 |
| `decision.reasoner.model` | selector ID / 无 | 慢速模型 binding；`gemma4-reasoning`。 | 与 controller 相同的校验，且必须与之不同。 |
| `decision.max_invalid_replies` | 整数 / 模型回复数 | 连续错误模型输出导致 Mission 失败的上限；3。 | 至少 1；传输重试不计入。 |
| `decision.step_timeout_s` | 有限数 / 秒 | Primitive 步骤出口超时；30。 | 大于 0；超时不等于 EOS 完成。 |
| `monitor.heartbeat_interval_s` | 有限数 / 秒 | Backend 健康轮询间隔；5。 | 大于 0；图边频率不能代替此安全监控节奏。 |
| `monitor.failure_threshold` | 整数 / 轮询次数 | 连续心跳失败后升级的门限；3。 | 至少 1；只有验证恢复后才重置。 |
| `monitor.stall_timeout_s` | 有限数 / 秒 | 运行中 Mission 没有新 primitive 结果的偏离门限；安装默认 150。 | 大于 0；进行中的模型推理不算 primitive 停滞。 |

`server.port` 是进程级 TCP 整数端口（1–65535，安装默认 16060），不是阶段设置。`gateway.transport` 是 `tcp` 或 `uds`，依赖传输类型的字段在 [IsOS 配置适配器](isos-config.md)中明确说明。Selector 的稳定 `value` 存入图，真正的包或配置路径在图外解析。选项须含标签、描述、可用状态和不可用原因；UI 应显示不可用的已保存值，并在更正前阻止 Apply。本次版本使用已配置的模型 binding；由包管理的权重选择留待后续扩展，本次不添加 `egia.tensor`。

## 准入、错误与兼容行为

**正常：** 完整发现代际使 link ready；证据经过 perception/context；已选 Mission 产生提案；gate 放行一个精确 primitive 或输出拒绝；EOS 终结结果回到证据链。**空输入：** 没有 Mission 或可选观测时阶段等待，不编造数据。**错误、重复、过期：** 所属阶段用稳定事件码、节点 ID 和关联 ID 拒绝。**不可用、部分结果：** 必需的发现阻止 ready，可选模型/记忆可降级。**超时、取消：** 在 EOS 终结证据到来前保留 pending/failed，不能推断物理完成。**重启：** 读取 checkpoint，创建新的 run ID，重新发现 EOS；紧急锁存必须经授权重置。

图边调度器可以优先和限速传递证据，但 gate 始终依据当前已验证状态准入。有界 observation/context 队列溢出时要计数并记录日志；安全和终结结果事件不能被静默丢弃。图不能把模型提案直连 link，也不能配置任意紧急发送方。唯一的紧急旁路是 monitor 的专用停止出口。

## 接口验证

[Q03 所有者资格验证设计](../design/q03-qualification.md)规定现有两条 A04 证据路径的测量。负载与指标是测试证据，不增加阶段端口、QoS 设置或安全路径。所选安装 wheel 与跨所有者回放仍是发布门槛。

[Q01 Vision 证据入口](vision-evidence.md) 是独立的进程所属只读检查路径。它不增加第八阶段、图端口、A04 `resource` 图像解释、提示输入或执行器路由。Q01 中 gate 帧不进入此阶段调度器。

为每个端口和设置 descriptor、未知字段/增量字段兼容、旧 `@v1.0` 只读解释、未来类型拒绝、不可用 selector 值、错误/过期/重复事件建立 contract fixture。EOS stub 集成测试须证明图线无法绕过 gate，断线不会生成成功结果。实现后把完整输出保存在 `logs/test/<run-id>/`。
