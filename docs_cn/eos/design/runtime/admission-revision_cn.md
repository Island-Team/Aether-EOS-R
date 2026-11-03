# R03 执行器受治理准入设计

[English](../../../../docs/eos/design/runtime/admission-revision.md) · [所有者契约](../../interfaces/grpc_cn.md#r03-执行器受治理准入目标契约) · [硬件契约](../../interfaces/hardware_cn.md#r03-dispatcher-准入目标契约) · [首次使用重放](../../develop/admission-revision_cn.md) · [冻结用例](../../../../tests/fixtures/r03_admission_revision/README_cn.md)

**R03 经合并后的 EOS 与 IsOS 所有者验收。** R03 扩展已合并的 R02 网关和现有持久 Dispatcher。不增加 protobuf 服务，不执行图连线，不实施 R04 宿主技能或 R07 浏览器结果传输，也不宣称 R05/R06 的 MuJoCo 物理隔离与停止顺序。IsOS 拥有保存图字节及活动修订；EOS 拥有已准入投影、实时健康、命令准入和 journal。

## 固定一个活动代际

IsOS 在启动 EOS 前验证 schema-2 生成快照。在受管理路径中，EOS 从 `data/generated/<deployment>/v2/<revision>/<generation>/` 所选叶目录读取 `manifest.json`，要求 profile 路径指向其下清单列出的普通 `eos/graph.json`，拒绝符号链接及路径逃逸，核对刚读取的字节与清单 SHA-256 条目，并将清单中 64 位小写十六进制 `revision` 保留为 IsOS 保存整图的 `active_revision`。清单代际和所选叶目录先通过 IsOS 既有启动前验证；EOS 在 gRPC/控制就绪前重复文件与修订校验。Vision/route-status 既有的 `projection_sha256` 是 EOS 投影字节摘要，绝不能充当保存图修订。受管理模式同时要求 `EOS_CONTROL_GRANTS_FILE` 与符合 IsOS `/v2/<revision>/<generation>/eos/graph.json` 布局的所选 profile 路径；只提供其中一项时启动失败。两项都有时，EOS 要求有效 `--run-id` 与验证文件 `run_id` 精确匹配；schema-2 清单或验证文件缺失、损坏、符号链接或不匹配时，在 adapter 构造、listener 绑定及 ready 事件前失败。可选的公开 `--run-id` 单独仅是关联值，独立测试亦可使用。IsOS 仍可不启动此执行器路径而检查并显式迁移旧保存图。两个受管理输入均没有的独立/mock profile 即使带有 `--run-id`，也只能只读发现且运动能力不可用，不能宣称 IsOS `active_revision` 或启用受管理执行器写入。

当前运行时每进程只有一份不可变图。每个执行器入口在最终释放闸门处、包括排队等待后，将调用方期望的活动修订与已准入值比较。保存草稿变化而不 Restart 不改变 EOS 活动值。Restart 时旧 listener 先停止接收，新写入失败，然后新 listener 发布完整代际；过期客户端必须重新发现并使用新活动修订。R03 不热替换进程内图。旧 `1.0.0` 图及不带 schema-2 清单的旧生成布局仍可安全读取、检查并显式迁移，但旧布局没有经验证的保存图活动修订，R03 生产执行器写入须关闭失败，直到图经按修订固定的布局 Apply。历史图和 protobuf 身份不改变。

## 将权限绑定到调用方

回环 TCP、调用方给出的源路径和图连线都不能认证进程。受管理运行中，IsOS 在私有 runtime 目录创建每次运行由 OS 密码学随机源产生的独立 `egia-control`、`operator-control` 授权，只把目标授权交给受监管 EGIA 或 EOS 浏览器 bridge，并通过所有者契约定义的 `EOS_CONTROL_GRANTS_FILE` 私有启动输入把验证文件路径交给 EOS。IsOS 签发与启动交付由其所有者 `interfaces/` 契约定义，并通过 IsOS PR #52 合并；合并后全新主目录重放验证其向 EOS 的交付。图字节、生成配置、能力描述、日志和公开元数据都不含授权秘密。EOS 以常量时间验证 Bearer 授权并映射为固定角色与范围；同 UID 进程仍处于本机 OS 信任边界。缺失、错误、过期或跨运行授权在预留和 adapter release 前失败。EOS 夹具在聚焦测试中注入确定性授权；合并后的 IsOS 所有者切片和全新主目录重放提供生产边界证据。发现及读取 RPC 对现有 EGIA 类型化客户端保持无授权可用。A02 在取得受管理授权后为 EGIA 写入添加元数据。已合并的 EOS bridge 注入其私有操作员授权并拒绝浏览器自带授权；R07 仍负责完整浏览器路径及结果后续查询验收。

`egia-control` 只能经受治理 gRPC 提交运动；`operator-control` 只能经私有 `primitive_typed` bridge 路径提交运动，其 gRPC 运动 Submit 被拒绝。两者都由 Dispatcher 解析精确活动 primitive binding。EOS 校验 binding 与角色；A02 须证明 EGIA 提交前经过安全阶段。`egia-control` 可请求正常整机 `robot.stop`；两个角色均可对精确已准入设备 binding 请求 `motion.stop`，并可凭有效授权在期望修订缺失或过期时调用 `safety.estop`；草稿不一致或运动队列满不能阻断紧急停止。运动 inhibit 期间只有 `operator-control` 带显式 `x-eos-stop-recovery: true` 标记，才能以全新 ID 授权整机恢复停止；必须覆盖旧未解决目标和当前目标并集，不能清除紧急 latch。`egia-control` 及未标记的操作员停止均不能通过恢复闸门。没有第三种公开安全授权。内部关闭独立调用 Dispatcher 安全接口。任何授权都不允许原始硬件路径、图编写的技能序列或未声明 primitive。角色与目标权限依据已准入注册表检查，不相信客户端字符串。

## 准入与持久身份

每个公开运动入口构造一份规范命令：命令 ID、IsOS 部署 `run_id`、调用方角色、服务端证明的入口、保存图活动修订、primitive/版本、精确 binding 来源、精确硬件目标、类型化参数和单独校验的单调截止时间。同一个 Dispatcher 方法在任何可能物理释放前持久预留此身份。凭据字节和传输 request ID 不进入持久签名；稳定角色/范围与入口、IsOS `run_id` 及活动修订进入签名，单次尝试的期限不进入重复签名。同一 IsOS 部署运行内 EOS 子进程重启保留运行 ID 与授权，同一 ID 和规范请求返回保留状态。新 IsOS 部署运行轮换运行 ID 和授权；旧授权失败，新授权不能把旧运行结果当作本次结果查询，复用旧 ID 仅冲突而不释放；修订、角色、binding、目标或参数变化产生冲突，绝不释放。已授权所有者在新图活动后仍可查询旧操作，但新命令不能使用旧修订。已释放的 `OUTCOME_UNKNOWN` 对该 ID 是终态，不会自动重试。旧 journal 缺少新身份字段时应封存，不能从当前图推断。

停止记录持久保存同一所有者角色与运行 ID，以及不同的 `MotionStop`/`RobotStop`/`SafetyEstop` 种类、精确设备或机器人目标、保存修订（仅已认证 estop 可为空）和恢复标记。同 ID 跨角色停止或改变恢复标记产生冲突，不能返回另一角色的旧结果。Get/Watch/Cancel 要求 journal 角色/范围及同一运行；旧运行结果不得展示为本次成功。预留后，Dispatcher 检查释放协调器、安全锁存和 inhibit、精确 binding 与 adapter kind、活动修订、角色授权、节点生命周期与新鲜健康、必需的类型化观测、有限数值与配置单位，以及剩余单调期限。调用 adapter 前持久写入 `Releasing`。排队过期或释放前 journal 写入失败时 adapter 调用数为零。终态结果与同一命令 ID 持久保存；物理释放模糊、终态落盘丢失或 `Releasing` 后崩溃成为 `OUTCOME_UNKNOWN`，并在现有显式停止恢复证明静止前抑制运动。不能把传输超时或断线视作物理成功或取消。`motion.stop`、`robot.stop`、`safety.estop` 保持 R02 中不同的 journal 类型与保留 stop 通道。

部署路径关闭私有 socket 的原始 `dispatch`、旧 `manual` 与 `primitive`、图序列 `skill_execute`、`skill_result`、未 journal 化的 `stop` 与 `manual_stop`，以及原始 `cancel`、`result`、`skill_cancel` 路径。`GetOperation`、`WatchOperation`、`CancelOperation` 与后续 R07 私有结果查询均要求有效授权匹配该 ID journal 记录的角色/范围；跨角色访问不泄露或修改操作。生产中的 `skill_cancel` 不能调用未 journal 化的 `stop_node`。私有 runtime Unix socket 保留仅所有者可用的关闭入口，以 mode-0600 socket、mode-0700 父目录及与 EOS 所有者比较的 peer UID 校验保护；IsOS Supervisor 是预期调用方。HTTP bridge 不能转发 `shutdown`，非所有者不能终止 EOS 或停止 adapter。R06 单独证明执行器停止先于 MuJoCo 终止/回收；向进程组发送 SIGTERM 本身不足以证明。历史类型仍可解析，本地测试可用进程内 helper，但部署调用方不能以此形成第二条执行器路径。R01 证据队列不承载 command/result/固定安全/stop 载荷；证据队列满不能丢命令。`primitive_typed` 保留为 R07 浏览器 bridge 路径，但其 R03 侧必须使用统一准入命令，而非旧的无治理 helper。R07 拥有浏览器 Origin/CORS、修订展示和关联的 `InProgress` 结果后续查询。

## 验证边界

[冻结用例](../../../../tests/fixtures/r03_admission_revision/cases.json)覆盖受管理修订来源、缺失与过期修订、缺失与错误授权、精确 binding 与类型化界限、假时钟健康/观测/期限、阻塞队列重检、重复与冲突身份、journal 故障与重启、并发停止及所有旧绕过路径。单元测试使用假时钟和单次调用 adapter；边界测试使用真实 Rust listener/私有 socket 与 stub adapter。EOS 进程测试构造 schema-2 快照和验证文件夹具；合并后的 IsOS 签发及启动交付已通过单独的边界重放，记录于首次使用页。物理控制器去重和模拟器停止/回收时序仍属 R05/R06，R03 不能从 mock 测试宣称这些属性。
