# R02 Rust gRPC 网关设计

[English](../../../../docs/eos/design/runtime/grpc-gateway.md) · [所有者契约](../../interfaces/grpc_cn.md#r02-冻结的-rust-子集) · [首次使用重放](../../develop/grpc-gateway_cn.md) · [机器用例](../../../../tests/fixtures/r02_grpc_gateway/README_cn.md)

**状态：** 默认 Rust runtime 已实现经过评审的 R02 gRPC Gateway 与 CapabilityService 子集，服务现有 EGIA typed client；R03 在同一 Dispatcher 上实施命令准入。已发布 C++ 服务仍是兼容参考。[跨仓库任务](../../../../docs/island/implementation-todo.md)中的 R06 停机顺序及 M3 最终验收另有门槛。

## 组合与发布

在 `aether-runtime` 增加有界 `tonic`/`prost` 服务模块，从现有 protobuf 文件生成 Rust stub。锁定生成器版本并验证 Google API 导入；以 descriptor set 对比既有 wire 身份，防止构建悄然修改字段号。五个 Rust 可执行文件名及 crate 版本 `0.2.0` 不变；公开 `eosd` 启动器仍调用 `aether-runtime`。CLI 增加默认 16200 的回环 gRPC 端口及可选、受所有者权限保护的 Unix 路径，两者均区别于现有私有 JSON 控制 socket。没有明确部署策略时拒绝通配绑定；重复或不安全 socket 路径在就绪前失败。

启动顺序：profile/注册表和生成配置锁定验证 → 排他持久 journal → adapter 和直接 MuJoCo 子进程 → 不可变 Gateway 投影 → gRPC 绑定 → 聚合 health `SERVING` 及既有所有者 ready 事件。当前实现遇到不完整准入会在绑定前失败，不会发布准入前 listener 或 ready 事件。若将来先绑定，health 必须保持 `NOT_SERVING`，所有必需方法返回 `UNAVAILABLE`。投影保存机器人身份、设备路径映射、精确控制 binding、资源注册、能力目录和 `started_at`，在 listener 发布前只构建一次；R02 没有进程内图刷新或 `Arc` 交换。`GetStatus` 叠加实时健康，不混合不同代际身份。失败的候选代际不能替换当前完整投影或发布半个目录；实时健康可独立降级。停机前先把 health 设为 `NOT_SERVING`，拒绝新写入并排空有界 stream。私有控制 socket 和浏览器桥仍是同一 Runtime/Dispatcher 的调用方，不另建执行器路线。

现有图只有机器人 `id`/`name`，没有物理型号字段。`RobotDescriptor.model` 使用字面量 `eos-runtime`，说明文字透明标识图运行时；设备 class/part 来自已准入硬件节点 kind/placement，ID 和说明来自节点，状态与健康来自 adapter。仅规范化已注册、可验证的类型元数据。就绪要求 `Discover`、`ListResources`、`GetStatus` 和 `ListCapabilities` 相互一致且可被 EGIA 解析。尤其 `GetStatus` 必须包含与 Discover 相同的 `robot_id` 和 `robot_name`，以及非空 `status`、`version`、`started_at`；EGIA 重新发现时检查 `robot_name`。EGIA 先执行空服务名 health `Check`、以上四个调用，再调用 `SkillService.ListSkills`；在 Rust 技能目录建立前后者必须返回 `UNAVAILABLE`，以进入现有客户端的可选技能路径。返回 `UNIMPLEMENTED` 会使 EGIA 启动失败。缺少必需响应时不能宣称 ready，也不能虚构设备、primitive、资源或健康采样。

本进程只读已 Apply 图；新图代际需要进程重启，因此四个启动发现调用不会跨越进程内图替换。实时健康和能力可用性可改变，但 binding 身份不变。若以后支持热刷新，必须完整发布代际并提供客户端重试边界，不能在多个独立 RPC 间返回混合目录。

已实现的 Rust 投影为每个已准入投影使用一个后台工作线程采样 HAL 健康及生命周期状态。采样器同时最多有一次 adapter 探测，因此阻塞的所有者 socket 不会占用 tonic 工作线程，也不会随读取负载复制探测。投影构建最多等待 200 ms 获得初始完整样本；样本缺失时报告不可用，但已准入身份及 listener 保持完整。发现、状态、能力列表与相机预检查只读取保留样本。每次探测在调用健康和状态接口之前记时；缓慢的状态响应不能刷新旧健康。失败、缺失或超过 `min(profile stale_after_ms, 1000 ms)` 的样本使可用性降级；新鲜探测恢复可用性。完整新鲜投影以 `eos-sample-age-ms` 给出最旧节点样本的年龄，缺失或过期时省略该字段。ReadResource 和 CaptureFrames 使用保留产品或帧的年龄。停机阻止采样器之后再发布，无需等待阻塞 adapter 调用；Dispatcher 在写入释放前仍重新检查物理准入。gRPC 工作许可为十六个运动、一个受控停止及一个紧急停止分别保留独立路线；运动工作线程饱和不能占用两条停止路线。

## 读取与有界 stream

`ListResources` 只列出已注册的生产者，每项包含 `robots/{name}/...` URI 和 `class`。当前 EOS 相机帧按需采集，不是 world 资源；当前图没有 world/detection/ASR 生产者，因此在真实生产者出现前这些类保持缺席。`ReadResource` 校验精确已列出 URI、schema、健康、采样年龄 ≤1 秒及响应上限，随后返回 Struct。`WatchResource` 订阅同一 URI 的变更，在已校验产品改变后发送 `ResourceChanged{uri}`，不携带图像原字节。每个 watcher 有界队列使慢消费者以 `RESOURCE_EXHAUSTED` 断开。没有游标。EGIA 当前只对 `NOT_FOUND` 和 `UNAVAILABLE` 自动重新发现，然后新建 watch 并 `ReadResource`。慢 watch 的 `RESOURCE_EXHAUSTED` 会交给调用方；除非另行评审 EGIA 重试集合变更，否则必须由调用方显式重试和重新读取。通知不等于完整状态采样。

`CaptureFrames` 一次解析已准入相机设备 ID，校验机器人名称和实时健康，从 EOS 自有两帧缓存读取 1 或 2 个不同的新鲜帧。使用已有 `VisionFrame::encode_jpeg` 或已校验 JPEG preview；检查 SOI/EOI、尺寸、时间戳及不同采集身份。响应 ≤16 MiB，每张 JPEG ≤8 MiB。缓存等待及编码均遵守截止时间和取消；超时不能留下无界任务。缓存不可用返回 `UNAVAILABLE`，响应过大返回 `RESOURCE_EXHAUSTED`。

## 操作与持久身份

只有受支持、已注册的 primitive→`Movement` 转换和真实控制边才能产生运动能力 descriptor。Stop descriptor 是另外的 Dispatcher 控制操作种类，目标范围不同。每项规范名称、逻辑 primitive/version/device 与 `available` 对应精确一个 Discover binding。图声明 wire 不执行命令。R02 的可执行目录可小于历史 C++ 服务，但对 EGIA 必须完整且内部一致。R02 实现三种不同的 journal 化 Dispatcher stop：`MotionStop{device}`、`RobotStop`、`SafetyEstop`；普通运动不健康时这些安全 binding 仍可用。它们与运动共享 operation ID 和单次释放规则，但有各自的停止目标、阶段转换、终态确认及 ack 结果。现有 Rust Dispatcher 支持 base velocity/relative pose 与 manipulator joint/end-effector movement，尚未实现所有历史 v1 primitive 或完整 acknowledgement。

初始转换表为 `body_velocity → base.velocity@1.0.0`、`relative_pose → base.navigate@1.0.0`（仅 **relative**）、`joint_positions → arm.movej@1.0.0`（仅 **absolute** 位置）、`end_effector_pose → arm.movel@1.0.0`（仅 **absolute** pose）。每项均以相符 profile mode、精确控制边、调用方参数 schema 和数值上限为前提。不得仅因 EGIA 知道这些名称就公告绝对 base 导航、相对机械臂目标或其他历史 primitive。运行时关闭仍调用 `emergency_stop`；公开 stop 能力使用自己的 journal 化命令路线。`MotionStop` 只停止一个已准入部件，已停止视为成功；`RobotStop` 对全部活动执行器扇出受控停止，不接合紧急 latch，并报告有界 `result.parts`；`SafetyEstop` 接合已配置紧急路径和 latch。只有 adapter 或紧急路径确认终态停止后才返回 `SUCCEEDED`。已知部件失败使整体 `FAILED`；释放后未确认使结果为 `OUTCOME_UNKNOWN`。现有 `stop_node` 和 `emergency_stop` 本身没有公开命令 journal 身份或足够的终态结果，不能直接暴露。只有具备完整参数 schema、limits 和依赖健康投影的 binding 才能公告。

扩展同一持久 Dispatcher journal 记录，使其保留规范请求签名、primitive/version、精确目标/binding、准入状态和重启后 `GetOperation` 所需的 typed ack 数据。安全读取/迁移旧记录，绝不猜测缺失 primitive alias。缺少足够规范 ack 元数据的旧记录保持封存：对该 ID 的 `GetOperation` 返回 `DATA_LOSS`，启动报告 journal 恢复降级，且绝不再次释放。新的公开 operation 记录必须始终具有构造终态 ack 所需的元数据。当前 journal 把每个 `node_path` 都校验为硬件路径。R02 引入版本化、有标签的 `JournalTarget`：`Hardware{node_path}` 继续使用现有硬件路径规则，`Robot{robot_id}` 必须匹配已准入 profile；签名内另有 `Movement`、`MotionStop`、`RobotStop`、`SafetyEstop` 四种 `OperationKind`。迁移旧 journal 记录时只映射到 `Hardware`，不以虚构硬件路径表示全机器人命令。签名包含目标和种类，预留后均不可改变。全机器人 stop 命令不能按命令取消。重启时未终结的 estop 变为 `OUTCOME_UNKNOWN`，不再次物理释放；其持久 latch 保守地阻止新运动，直到另有明确授权的恢复路径。`RobotStop` 不设置该 latch。将现有 120 字节 ID 上限扩展到公开的 128 字节。预留前验证 ID 和请求，原子比较重复签名，随后在一次**逻辑**释放前持久写入 Prepared 和 Releasing。运动与 `MotionStop` 最多向一个 adapter 释放一次。`RobotStop` 在 Prepared 前按规范路径排序，快照全部已准入且可停止的硬件节点，把不可变列表连同 typed 机器人目标一起持久保存；随后在从 Releasing 前阻止新 Dispatcher 运动释放、直到验证完成或恢复才清除的独立持久运动 inhibit 下，对列表中每个部件最多调用一次停止。该 inhibit 不是紧急 latch。每次调用前在同一 journal 记录中标记该部件 attempting，调用后持久记录已确认结果。`SafetyEstop` 只接合一次 latch，经自己的已配置紧急路径扇出，也属于一次逻辑释放。部分扇出时崩溃，整体恢复为 `OUTCOME_UNKNOWN`，只保留已确认的逐部件结果，其余标记 unknown；该 ID 的已完成和未完成部件都不会自动再次调用。已准入注册表最多允许 64 个可停止部件，使快照及终态 `result.parts` 都装入有界 journal 记录。每个经脱敏的部件原因最多 256 字节。注册表超限则不得进入 SERVING；释放前检测到序列化溢出返回 `RESOURCE_EXHAUSTED`，可能已释放后的溢出保留 `OUTCOME_UNKNOWN`，绝不重试。gRPC adapter 不持有 backend handle。完全重复的并发调用共享一条记录，冲突复用返回 `ALREADY_EXISTS`。释放前 journal 写入失败阻止动作；若可能已经释放，则记录或恢复为 `OUTCOME_UNKNOWN`，绝不重试。

同一 journal 保存已同步的 RobotStop inhibit 控制状态；它在 Releasing 前设置，所有运动释放（包括并发 Submit）都要检查。只有不可变快照中每个部件及当前全部已准入可停止部件都确认终态停止后，才能持久清除。已知部分失败、不确定释放、崩溃或清除写入失败均保持 inhibit。重启后部分扇出记录变为 `OUTCOME_UNKNOWN`，原 ID 只可查询。新的、按本地策略授权的 RobotStop ID 可通过快照尚未解决的旧目标与当前已准入可停止节点的并集恢复；确认所有目标后才清除。旧目标无法验证时保持 inhibit，直到显式授权维护提供等价逐目标停止证据。紧急 estop latch 独立存在，不能通过此恢复清除。inhibit 活动时，完整 gRPC 接口可为安全发现和停止恢复报告 health `SERVING`；`GetStatus.status` 与机器人 health 为 `degraded`，运动能力不可用，运动 Submit 在释放前拒绝。Stop 能力仍可用。这里的服务可用不代表物理运动就绪。

将释放协调器实现为 Dispatcher 所有、写方优先的运动许可屏障，与 journal 互斥锁和紧急停止 latch 分离。运动先取得许可，在屏障下检查 inhibit/截止时间，再调用 `begin_release` 持久写入 `Releasing`，直到 adapter 调用及其终态或物理边界隔离完成才释放许可。RobotStop 原子关闭新运动许可后持久设置 inhibit；inhibit 同步完成后等待所有旧许可结束，包括已写入 `Releasing` 但在 adapter 前暂停的运动，随后才扇出。等待或调用 adapter 时不持有 journal 互斥锁。调用返回 `Accepted`、`Ambiguous` 或可能已物理释放后的错误，即使线程结束也留下未解决代次。停止路线必须获得 adapter 证明的静止且无待执行命令，或在物理写入边界隔离该代次，才可宣称终态停止；否则在有界截止时间后成为 `OUTCOME_UNKNOWN`，inhibit 持久保留，不能凭一次已停观测清除。崩溃后从 journal 重建未解决的 `Releasing` 运动，在宣布运动可用或清除任何 RobotStop inhibit 前建立 adapter 代次隔离或等价证明。独立的 `SafetyEstop` 路线仍可抢先执行，不等待该屏障。确定性测试在运动同步写入 `Releasing` 后、调用 adapter 前暂停，启动 RobotStop，确认停止扇出与清除均不能越过该运动，再释放运动并确认有序停止；另一分支使运动结果模糊，核对 inhibit 与 unknown 恢复。

在 Prepared 前校验最多 64 个目标的快照，并按每个部件 256 字节原因的上限预留有界结果空间；实际原因在返回时脱敏并截断。若 Releasing 前序列化超限，返回 `RESOURCE_EXHAUSTED`，adapter 调用数为零且不改变 inhibit。若可能已经调用后写入或结果序列化失败，保留 `OUTCOME_UNKNOWN` 和 inhibit，不重试该 ID，且只保留已持久确认的逐部件结果。并发运动在扇出中、释放前注入溢出与可能释放后注入溢出须分别测试。

通过一个纯投影将 `DispatchResult` 转为 v2 Operation 和规范 `primitive.ack`，包括匹配的 ID、primitive/version、accepted、status、sequence 和有界 result。不可从现有未区分的运动结果编造 `FAILED` 或 `TIMED_OUT`；R02 stop 命令有自己的已确认 `FAILED` 结果，R03 可增加可辨的运动原因。`WatchOperation` 先发当前记录，再最多发一个终态。watch 断开不取消或重试命令；恢复使用相同 ID `GetOperation`。`CancelOperation` 经 Dispatcher 定向取消，不能停止同设备的后续命令。显式 stop 能力和关闭路径负责钳制运动。现有同步 Dispatcher 可立即返回终态，watch 契约仍支持未来 pending adapter。

## 错误、上限与验证

传输错误与终态领域结果分离。[所有者契约](../../interfaces/grpc_cn.md#r02-冻结的-rust-子集)固定错误映射、截止时间、帧和 stream 上限。在创建工作前限制 gRPC 解码、Struct 嵌套、URI/ID 长度、方法并发、队列及响应字节。`x-request-id` 只用于日志关联；共享 JSONL 事件不得包含参数、图像和环境。Submit 后连接断开对 EGIA 是不确定状态：服务端必须按 ID 保留命令供 `GetOperation` 查询，不能推断未执行。

[机器用例](../../../../tests/fixtures/r02_grpc_gateway/cases.json)要求假时钟新鲜度与截止时间单元测试、protobuf/EGIA stub golden 对等测试、真实 Rust gRPC listener 与所选 EGIA typed client 集成、跨 journal 重启的重复/并发/冲突 Submit、丢失响应和 watch 重连、非法 schema 与帧上限、慢 watcher 和取消、部分发现刷新、冷资源、backend 丢失以及停机 health/stop 顺序。格式、Clippy、单元/集成测试和构建完整输出保存到 `logs/test/<run-id>/`，首次使用指南记录精确命令。固定工作负载测出吞吐量、中位数和 p99 前不作性能断言。
