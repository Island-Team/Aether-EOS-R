# 0.x 详细实现 TODO

[English](../../docs/island/implementation-todo.md) · [层连接与数据流](layer-dataflow_cn.md) · [里程碑](implementation-milestones_cn.md)

**除所有者页面明确标记现有基础外，下列未勾选项均为计划中。** 本页协调 EOS `0.2.0`、IsOS/EGIA/IsosUI `0.1.0` 和 `island/mujoco-sim@0.1.1`。 [连接图](layer-dataflow_cn.md)定义路径所有权；各所有者的 `interfaces/` 页面定义字段。只有夹具、失败路径、相关停止/重启、英中使用指南以及 `logs/test/<run-id>/` 的完整输出通过审查，才关闭任务。编号只是计划引用，不是图或线协议身份。产品 WorkflowService、ActionList DAG 和图中技能序列不属于本轮目标。

## M1——图、目录与兼容

| ID | 所有者；依赖 | 实现和验收证据 |
| --- | --- | --- |
| [x] **G01** | IsOS + EOS；起点 | 清点当前所有图/类型 ID 和 sidecar 使用者。固定 `1.0.0`、精确已知 `@v1.0`/`@v1.1`、未知未来版本、旧任意命令进程节点夹具。断言既有安全行为；不使用版本通配推断兼容。 |
| [x] **G02** | IsOS；G01 | 按图版本分派解析器；只对精确映射的旧身份生成内存投影，保留原始图值供保存读取和执行；保留原字节 SHA 修订，给出规范路径诊断，未知版本只读。读/校验测试证明保存字节与修订不变。 |
| [x] **G03** | IsOS；G02 | 增加显式迁移预览和按修订比较并交换、原子写入 `0.1.0`；保留精确包锁定，未批准映射前不改旧身份。拒绝过期修订和旧 `command` 进程执行；活动修订直到 Restart 前不变。 |
| [x] **G04** | EOS + 包所有者；G01 | EOS 与已注册包侧文件贡献节点描述符；IsOS 汇总而不重新定义 EOS 类型。四个历史 EGIA 设计端口定义仍来自冻结的 EOS 目录副本，直到 EGIA 发布自有目录。注册带非空 `NodeType.description` 的新 `@v0.1` 类型及完整设置/端口/能力/生命周期元数据；旧 sidecar 仅有显示用回退。保留 protobuf 编号、JSON 名称、ABI 符号和路径，逐类型测试目录夹具。EOS/包的 M1 目录与旧版回退已实现。 |
| [x] **G05** | IsOS + EGIA + IsosUI；G02–G04 | 公告图/类型/特性支持集合，安全读取精确已知旧 ID。UI 草稿保留兼容未知字段，未来版本只读。EGIA 旧设计 ID 不获得新执行路径；读取器夹具断言各所有者契约规定的准确路径与代码；不同读取器的路径写法可能不同。 |
| [x] **G06** | IsOS + EOS；G04 | Apply 前分别校验位置、来源/输入数据类型 ID 完全一致、已注册单位注释的一致性、传输、基数、必填、循环、引用及进程依赖；不做隐式单位转换，并返回稳定且指向具体连线的拒绝诊断。实时设备健康由 EOS 运行时拥有，不阻止编辑或 Apply。分类为 EOS 核心内、EGIA 核心内、声明服务桥接或仅设计边；拒绝在固定安全路径上由用户设置 QoS。Apply 前拒绝 proposal→执行器和包贡献的绕过路径。 |

## M2——包、选择、配置和日志

| ID | 所有者；依赖 | 实现和验收证据 |
| --- | --- | --- |
| [x] **P01** | IsOS；G02/G04 | 将精确注册表发布安装/链接到不可变仓库；验证摘要、平台、描述符、可执行文件、资源包含及节点贡献。成功时记录精确包身份与已验证收据来源，验证或加载失败时记录脱敏原因。安装不改变保存图或活动进程；Apply/Run 时篡改、符号链接和路径逃逸失败。 |
| [x] **P02** | MuJoCo 包 + IsOS；P01 | 从已验证资源发布场景/设备稳定 ID、描述和包专属节点集；校验场景类型、设备映射、MuJoCo 库和不可用原因。换场景需新图修订和子进程重启。 |
| [x] **P03** | IsOS；P01/P02 | 按 `(node_path, setting)` 返回 value、label、description、availability、reason；Apply 时针对当前草稿和精确包重新解析。丢失的保存 ID 可见但不能应用；将来资源选择器复用契约，本轮不执行 `egia.tensor`。 |
| [x] **P04** | IsOS；G05/P01 | 从安装默认值及验证后的图叠加生成按修订固定的 EOS/EGIA 不可变配置。用户文件在 config，精确包仓库/生成快照/日志在 data，cache 分开，私有套接字在 runtime。密钥引用只对目标子进程解析，不写明文。 |
| [x] **P05** | IsOS + EOS + EGIA；G05/P04 | 启动前将**实际**图、包锁定和所需特性与读取器公告集合比较。诊断指出不支持的二进制、节点路径、类型/特性及支持集合；只有已知精确验证的兼容发布时才给出升级建议，不假设单一产品版本。 |
| [x] **P06** | 所有核心；P04 | 发出统一脱敏 JSONL：UTC 时间、级别、来源/核心/节点、事件码、run/correlation ID、消息和有界字段。包事件在有界字段内记录精确 `{id, version}` 及收据/摘要来源；部署运行尚不存在时，启动前故障使用管理器会话 `run_id` 和可选 `correlation_id`。每模块一条启动/就绪摘要，之后记录变化及终态；不记录原始帧、提示词、凭据或任意环境值。测试脱敏和高频抑制。 |
| [x] **P07** | IsOS + IsosUI；P06 | 按部署汇总日志并有界轮换、游标读取。实时状态为权威；启动前从管理器会话首条 Apply/包故障开始，启动后切换到部署运行首条就绪/故障，再按来源/节点/关联 ID 过滤。测试轮换、过期游标、新运行边界、采集器故障与恢复。 |

## M3——受治理执行和模拟器

| ID | 所有者；依赖 | 实现和验收证据 |
| --- | --- | --- |
| [x] **R01** | EOS；G06/P05 | 仅为 EOS 核心内已实现类型化边建立实时路由表：单调时钟 token bucket、可选 `frequency_hz`、有界队列、nice `-20..19` 到正权重的明确映射、加权公平服务及可见溢出/gap 计数。`expected_rate_hz` 只提示生产端。测试公平、限频、饱和和停止。 |
| [x] **R02** | EOS；G04/P05 | 增加 Rust gRPC health、Gateway Discover/ListResources/GetStatus/ReadResource/WatchResource/CaptureFrames 及现有 EGIA 类型化客户端所需的 CapabilityService SubmitOperation/WatchOperation/GetOperation/CancelOperation。保留已发布 protobuf 身份及错误；完整发现才就绪。契约测试格式错误、过期和重复请求、WatchResource 重连后重新读取、WatchOperation 重连后以 GetOperation 恢复、有界帧拍摄、终态 acknowledgement 身份及断线/未知结果恢复。 |
| [x] **R03** | EOS + IsOS；R02/P04 | 每执行器只有一条 dispatcher 准入路径；下发适配器前校验精确绑定、图修订、权限、健康、观测新鲜度、数值界限、期限和命令身份，记录终态/未知结果；重启后不自动重试结果不确定的已下发命令。路由边或 UI 请求不能绕过准入或静默丢命令。使用假时钟健康/新鲜度与持久日志重启测试。IsOS 按运行签发并交付调用方授权；仅 EOS 夹具授权不足以验收。下文记录经评审的所有者合并和合并后跨仓库重放。 |
| [x] **R04** | IsOS + EOS；P01/R03 | 实现 [EOS 宿主技能 ABI](../../docs/eos/interfaces/skills/native/host-abi.md)：显式验证库、descriptor/ABI 大小协商、受约束 worker、单次调用 broker 授权、schema/能力白名单、Rust SkillService ListSkills/execute 门面、取消和有界回收。未配置外部宿主、无效包或未声明能力必须在 guest 执行前拒绝；反向夹具证明此接口默认不可执行，不宣称完整沙箱隔离。以兼容夹具保留旧 C ABI 布局/符号；guest 不持有原始 runtime/dispatcher/device handle。 |
| [x] **R05** | MuJoCo 包 + EOS；P02/R03 | 增加受管理的严格套接字启动与健康特性确认，同时保留直接启动的旧 `2.0.0` 语义。校验图到设备绑定、操作种类和有限数值；按 `(node, command_id)` 去重并拒绝冲突重放。测试相同传输重发、损坏帧及不确定命令不重放。 |
| [x] **R06** | EOS + MuJoCo + IsOS；R03/R05 | 关闭准入，以专用有界 stop 通道中断进行中的运动/拍摄，取得底盘/机械臂确认或标记未知，再终止/回收模拟器。IsOS 逆序停止依赖的 EGIA/EOS 进程组。测试子进程崩溃、GUI 不可用、超时、强制停止及套接字清理。 |
| [x] **R07** | EOS + IsosUI；G05/R03 | 保留 Rust EOS loopback HTTP/WebSocket bridge，将浏览器控制与 Vision 有界请求转发到私有 runtime IPC。校验允许的 Origin/CORS、EOS 活动图中的已准入节点身份、body/frame 界限及 Vision 只读。首个 UI 原语演示沿用现有 `primitive_typed` bridge 路径，保留精确绑定/目标准入和关联结果，无需新 gRPC 身份。Vision 订阅保留修订检查；UI 在保存/活动修订不同时禁用控制。禁止来源和非活动节点在控制前失败。为 `InProgress` 后续查询通过 bridge 增加有界且按权限校验的命令结果读取；当前浏览器白名单不包含 runtime `result`。测试类型化绑定、结果所有权、bridge 故障及 Stop 可见性。 |

## M4——智能体阶段和操作工作台

| ID | 所有者；依赖 | 实现和验收证据 |
| --- | --- | --- |
| [x] **A01** | EGIA；G04/P04/R02 | 在单个 Python 进程内实例化 link → perception → context → missions/decision → safety → monitor 类型化阶段节点，每个值只有一个所有者。保留现有任务/计划模型；模型服务在部署图外。逐阶段 mock 测空、格式错误、过期、重复和停止。 |
| [x] **A04** | EGIA；A01/G06 | 为符合条件的证据阶段边实现进程内传递：单调时钟限频、正 nice 权重、有界公平队列和可见溢出计数。必需的 result/health/latch 值与固定安全/停止路径不能静默限频或丢弃。以假时钟、过载及停止测试。 |
| [x] **A02** | EGIA；A01/A04/R03 | 将 link 发现/反馈连接 EOS，仅发布完整代；safety→link 和 monitor 紧急路径是固定运行时路由。证明图连线不能绕过 safety/EOS dispatcher，断线/超时不成为物理成功。 |
| [x] **A03** | EGIA；A01/P06 | 以 run/operation 身份关联阶段、任务、提议、准入、EOS 结果及 monitor 事件。由 checkpoint 重启，行动前重新发现，紧急锁存直到授权重置。测试降级就绪和日志顺序。 |
| [x] **U01** | IsosUI；G04/P03 | 按 core → layer → group → node 浏览目录，区分类型/实例描述；按元数据生成表单和 node-options 搜索选择器。不可用 ID 保留并阻止无效 Apply；测试键盘、屏幕阅读器计数和过期选项响应。 |
| [x] **U02** | IsosUI；G06/R01 | 显示/编辑每条边的 nice 优先级及频率上限，提供文本标签、图例和无障碍检查器。声明类型/单位及指向连线的验证或拒绝原因与 EOS 实时节点健康分开显示；未路由边标 `design only`，不能推断其运行时健康。保留旧图默认及无关字段，Apply 前经 IsOS 校验，测试过期修订和无障碍错误文本。 |
| [x] **U03** | IsosUI；P07/R02/R07/A03 | 独立停靠/浮动 control、monitor、Vision、log 面板；保存并限制布局，始终显示 Stop、断线及活动修订。轮询/流有界；测试窄屏、焦点/缩放、关闭后资源清理和本次运行游标。 |
| [x] **U04** | IsosUI + EOS；G04/G06/R03/R07/U01 | 鼠标悬停和键盘焦点显示限制在视口内、可浮于其他节点之上的信息卡，展示类型与实例描述、类型化端口/单位，以及仅由所有者报告的实时状态；不妨碍拖动和连线编辑。EOS 面板对活动图中每个底盘/机械臂原语绑定显示一项控制及其唯一目标和类型化参数帮助，再经浏览器 bridge 调用现有 `primitive_typed`。按命令 ID 显示选中/浏览器本地发送以及精确的 `Completed`、`Cancelled`、`Rejected`、`InProgress`、`OutcomeUnknown` 状态，不把图边当作执行。测试焦点/悬停、重叠节点、过期修订、错误绑定/载荷、bridge 故障、准入结果和 Stop。 |

M4 归属方实现证据：EGIA [A01 #93](https://github.com/Island-Team/Island-egia/pull/93)、[A04 #94](https://github.com/Island-Team/Island-egia/pull/94)、[A02 #95](https://github.com/Island-Team/Island-egia/pull/95)、[A03 #96](https://github.com/Island-Team/Island-egia/pull/96) 及[进程重启证明 #98](https://github.com/Island-Team/Island-egia/pull/98)；IsosUI [U01 #40](https://github.com/Island-Team/IsosUI/pull/40)、[U02 #41](https://github.com/Island-Team/IsosUI/pull/41)、[U04 #42](https://github.com/Island-Team/IsosUI/pull/42)、[U03 #43](https://github.com/Island-Team/IsosUI/pull/43)、[选定目录 #44](https://github.com/Island-Team/IsosUI/pull/44) 与[工作台刷新 #45](https://github.com/Island-Team/IsosUI/pull/45)。[选定 M4 重放](m4-affected-replay_cn.md) 验证历史合并归属方版本上的真实受监督任务与浏览器命令。该 IsOS 版本不支持同一运行内单独替换 EGIA 子进程。后来的 [final-17 Q01 重放](../eos/maintenance/q03-selected-matrix-evidence_cn.md#final-17-选定主题提交结果2026-11-03)针对选定主题提交通过了故障注入与 Restart；[经审查的 M4 完成记录](m4-qualification_cn.md)按这些固定输入关闭原定阶段／工作台条目，保留历史同一次运行内 EGIA 子进程替换限制，也不宣称公网 HTTPS 或 M5 发布验收。

## M5——集成验收与发布

| ID | 所有者；依赖 | 实现和验收证据 |
| --- | --- | --- |
| [x] **Q01** | 所有产品；G01–G06、P01–P07、R01–R07、A01–A04、U01–U04 | 从全新主目录重放：安装 → 节点/资源选择 → 校验/应用 → 启动/就绪 → 查看节点并调用一个活动原语绑定 → 观察/控制 → 本次运行日志 → 注入故障/重启 → 安全停止 → 显式迁移；包含旧图和不支持版本。每步检查保存/活动修订及命令/结果关联。 |
| [x] **Q02** | EOS；Q01 | 审计无用 `src/eos/` C++ runtime、launcher、构建分支、示例和测试的最后调用者。等效 Rust 测试通过后才删除旧代码和指南；保留活跃 CXX mock、适配器 C ABI 及 EGIA 使用的 EOS proto/schema。剩余 `v2` 示例/脚本改主题文件名，不改线协议身份。 |
| [x] **Q03** | 所有产品；Q01/Q02 | 运行格式、lint、单元/契约及安装后集成测试；完整输出在 `logs/test/<run-id>/`，记录命令、环境、失败和结果。固定负载基准报告吞吐、中位数、p99；先给实测基线再设目标。遵循[已审查的验收设计](q03-qualification_cn.md)。 |
| [ ] **Q04** | 维护者；Q03 | 审查合并各子系统 PR，确认远程集成分支，更新英中首次使用与发布说明，再只发布选定的 0.x 产品标签。历史 1.x 标签、图身份和独立协议 ID 保持不变。 |

Q02 的[已审查最后调用方证据](../../docs/eos/maintenance/q02-evidence.md)确认历史 WorkflowService、JSON setup、HTTP/SSE 与 REPL 路由及实体适配器仍有原生调用方。准确等价或经审查弃用前保留这些旧 C++ 源；没有组件满足删除规则。本切片隔离活动技能 SDK 与安装资源，修正过时的中英文指南，并在保持图字节与线协议身份不变的情况下重命名仍在使用的 `v2` 文件。Q02 的勾选表示完成审计与安全处置，不表示已删除仍有调用方的原生组件。后续跨仓库发布门槛由 Q03 负责。

## 最小集成矩阵

| 路径 | 必需的正向与反向断言 |
| --- | --- |
| IsOS 图 → EOS/EGIA 配置 | 新旧夹具在支持时启动；未知类型、过期修订、坏包或缺失特性在启动子进程前失败。 |
| EOS 相机 → gate → Vision/EGIA | 类型化帧来源和有界传递；过期帧、频率上限及队列溢出可见，不编造证据。 |
| EGIA 任务 → safety → EOS → MuJoCo | 一条已准入命令及关联终态；绕过、重复冲突、超时和未知结果安全拒绝。 |
| IsosUI → IsOS Apply 与 EOS 控制 | 不可用选项和过期修订阻止 Apply；控制仅对活动声明节点进入 EOS 准入。 |
| 所有核心 → IsOS 日志 → IsosUI | 过滤前能找到启动/就绪或首故障；脱敏、轮换和游标恢复保持运行边界。 |
| 停止/重启 | EOS 执行器停止先于模拟器终止；EGIA 新动作先重新发现；IsOS 明确报告强制/未知结果。 |

## 验证记录

先前仅修改文档的一轮经过独立契约审查，完整输出位于被忽略的 `logs/test/20261101-layer-dataflow-todo/docs.log`。本次合并根目录待办时，独立 agent 确认所有权：Apply 前的连线验证属于 IsOS，实时健康属于 EOS。已实现的 Rust 类型描述部分通过 `cargo fmt --check` 和 `cargo test -p aether-node-profile --tests`（20 项）；既有假时钟健康/新鲜度与持久日志不重试检查通过 `cargo test -p aether-dispatcher --tests`（16 项）。`python3 scripts/check_doc_links.py --all` 检查 290 个 Markdown 文件，零损坏相对链接。完整本地输出位于被忽略的 `logs/test/20261101-root-todo-merge/`。这些检查不代表剩余里程碑任务完成。

### M1 关卡证据

G01–G02 已通过 IsOS [PR #41](https://github.com/Island-Team/isos/pull/41) 合并；G02 精确旧图检查投影及共享准入补齐通过 IsOS [PR #44](https://github.com/Island-Team/isos/pull/44) 合并。G03 已通过 IsOS [PR #42](https://github.com/Island-Team/isos/pull/42) 合并。G04 已通过 EOS [PR #306](https://github.com/Island-Team/Island-eos/pull/306) 合并；G05 读取器分别通过 EGIA [PR #90](https://github.com/Island-Team/Island-egia/pull/90)、IsosUI [PR #35](https://github.com/Island-Team/IsosUI/pull/35) 和 EOS [PR #307](https://github.com/Island-Team/Island-eos/pull/307) 合并。EOS G06 契约通过 [PR #308](https://github.com/Island-Team/Island-eos/pull/308) 合并，IsOS G06 准入通过 [PR #43](https://github.com/Island-Team/isos/pull/43) 合并。各 PR 均完成独立审查及所有者检查，完整本地输出位于相应忽略 Git 的 `logs/test/20261101-*` 目录。

先前的跨仓库 G03 子集重放在 EOS `fb57a0c5fb83833f99899e0939f00b3865adaa8e` 和 IsOS `e45b75e459c1e6452f7da0f16cb675fb08d51ad7` 上通过：安全读取旧图后原字节 SHA 不变；未来版本及不安全图拒绝且不写入；过期迁移返回冲突；显式迁移写入 `0.1.0`；EOS 投影启动、重启、活动修订及程序变更拒绝均通过。完整输出：忽略 Git 的 `logs/test/20261101-m1-replay/g03-replay.log`。

M1 连线重放确认 EOS 核心内相机路径可用，且冻结目录中的 EGIA 核心内连线得到安全分类。启用 EGIA setup 的图仍会在 `/cores/egia` 处关闭失败，因为 EGIA 要求已验证的 EOS gRPC Gateway；该能力安排于 R02/P05。这是后续联合部署重放的依赖，不能据此宣称 M1 的 EGIA 运行时已就绪。目前 IsOS 的所有者适配器错误未保留 EGIA 的结构化拒绝详情，仍属 P05 诊断任务。

IsOS G06 合并后，受版本控制的 `tests/m1_cross_repo_replay.py` 首先完成 M1 连线关卡重放：EOS 运行时 `fb57a0c5fb83833f99899e0939f00b3865adaa8e`、IsOS `b650457baa08b5d231d44d8bd944370aae51955b`、EGIA `6b8b5c6d9e72c584e8632684e9ff75d7cb2d07db`、IsosUI `1f7af4059eeb8d4b56e02a844416db74f2eead7a`。EOS 相机连线的正向检查返回已分类声明。错误类型、越界优先级、固定命令 QoS 以及未声明的 EGIA guard → EOS Dispatcher 桥接，均通过 HTTP/CLI 返回相同的具体指针、代码与上下文；被拒编辑不改变保存字节或活动子进程。篡改保存图后的 Run 在更换子进程之前被拒。完整输出位于忽略 Git 的 `logs/test/20261101-m1-qualification-postmerge/replay.log`；此前精确 G06 分支重放位于 IsOS `logs/test/20261101-g06-edge-admission/full-m1-replay.log`。

IsOS PR #44 合并至 `09afc6b9109d0c321c53affd994b3efdfc0d3300` 后，新构建的 IsOS 与同样固定修订的 EOS、EGIA、IsosUI 通过扩展后的 M1 全关卡重放。安全旧图 GET 单独暴露只读检查投影，含精确候选映射与可选默认值；仅空白不同的保存字节生成相同投影，但来源修订不同。历史所有者契约下有效但无映射的控制节点、不安全进程节点、未知类型和未来图均不产生投影，Validate、Apply、Run 在各自契约路径拒绝且不修改保存字节。显式迁移、连线拒绝、投影启动/重启、篡改拒绝及活动修订检查也全部通过。完整输出位于忽略 Git 的 `logs/test/20261101-m1-qualification-final/replay.log`；IsOS 格式、Clippy、测试、构建、独立审查及首次使用输出位于其 `logs/test/20261101-g02-legacy-projection/` 与 `logs/test/20261101-g02-independent-review/` 目录。

复现时，将四个仓库检出到上述最终固定提交并构建，设置 `EOS_ROOT`、`ISOS_ROOT`、`EGIA_ROOT` 和 `ISOSUI_ROOT` 为绝对路径，再在提交 `0f2b6545` 上运行：

```sh
python3 tests/m1_cross_repo_replay.py --run --stage all \
  --eos-root "$EOS_ROOT" --isos-root "$ISOS_ROOT" \
  --egia-root "$EGIA_ROOT" --isosui-root "$ISOSUI_ROOT" \
  --expect-eos fb57a0c5fb83833f99899e0939f00b3865adaa8e \
  --expect-isos 09afc6b9109d0c321c53affd994b3efdfc0d3300 \
  --expect-egia 6b8b5c6d9e72c584e8632684e9ff75d7cb2d07db \
  --expect-isosui 1f7af4059eeb8d4b56e02a844416db74f2eead7a
```

### M2 所有者合并与验收证据

P01 已合并于 IsOS [PR #45](https://github.com/Island-Team/isos/pull/45)。P02 已合并于 MuJoCo [PR #18](https://github.com/Island-Team/isos-mujoco-sim/pull/18)、后续 [PR #19](https://github.com/Island-Team/isos-mujoco-sim/pull/19) 和 IsOS [PR #46](https://github.com/Island-Team/isos/pull/46)。P03 已合并于 IsOS [PR #48](https://github.com/Island-Team/isos/pull/48)。P04 已合并于 IsOS [PR #47](https://github.com/Island-Team/isos/pull/47)。P05 已合并于 EOS [PR #310](https://github.com/Island-Team/Island-eos/pull/310)、EGIA [PR #91](https://github.com/Island-Team/Island-egia/pull/91) 和 IsOS [PR #49](https://github.com/Island-Team/isos/pull/49)。P06 已合并于 EOS [PR #311](https://github.com/Island-Team/Island-eos/pull/311)、EGIA [PR #92](https://github.com/Island-Team/Island-egia/pull/92) 和 IsOS [PR #50](https://github.com/Island-Team/isos/pull/50)。2026-11-01 已逐项核对链接处于 `MERGED`，并确认精确合并提交。所有者夹具、反向测试和本地测试日志记录于所链接 PR 及所有者仓库。P01–P06 的勾选仅记录这些所有者实现已合并，不代表 M2 里程碑通过。

P07 已合并于 IsOS [PR #51](https://github.com/Island-Team/isos/pull/51)，提交为 `ace8f4c60b8a74b3a998fe2ae78da4c38afa2020`；IsosUI [PR #36](https://github.com/Island-Team/IsosUI/pull/36)，提交为 `bea957ce3a5b023f15727978a832186db135a159`。修正后夹具的 [M2 验收重放](m2-qualification_cn.md)在精确锁定的合并提交上分六批通过全部 28 条用例。当前证据为被 Git 忽略的 `logs/test/20261101-m2-review-final-v2/gate/aggregate.json`；每批都有摘要绑定的证明，`aggregate-tamper-negative.log` 记录三类篡改输入被拒绝。为保留这些证明，冻结夹具中重放前的状态标签保持不变。此结果仅验收 M2。EGIA Gateway 就绪属于 R02，受控执行器停止顺序属于 R03/R05。

### M3 所有者进度

R01 已通过 [PR #313](https://github.com/Island-Team/Island-eos/pull/313) 合并到 EOS 集成分支，合并提交为 `3469af893707ab6bbb1e138e2282aa2bff0d0bfa`。其实现、独立评审修正及共享扇出检查的完整命令和输出保存在 EOS 主检出目录下被 Git 忽略的 `logs/test/20261101-r01-routed-implementation/`、`logs/test/20261101-r01-review-corrections/` 和 `logs/test/20261101-r01-shared-fanout/`。

R02 已通过 [PR #314](https://github.com/Island-Team/Island-eos/pull/314) 合并到 EOS 集成分支，合并提交为 `7800c994c5813954f19cea09c08c6235144a76b7`。其经独立评审的[首次使用与完整关卡记录](../../docs/eos/develop/grpc-gateway.md)包括所选 EGIA 类型化客户端重放、35 用例映射、阻塞健康探测/读取并发、保留的 stop 通道和持久未知结果恢复。R02 勾选记录的是已评审的 EOS 所有者合并；R03 的验收另见下文，限定范围的 M3 完成条件现已通过，见下文。

R03 通过独立所有者评审，并经 EOS [PR #315](https://github.com/Island-Team/Island-eos/pull/315) 合并到 `refactor(v2)/runtime-main`，提交为 `dbd2e2c39a54236786577ace424c30e418ff3926`；IsOS [PR #52](https://github.com/Island-Team/isos/pull/52) 合并到 `master`，提交为 `df3c1f5692c9348c1cfb5b2ecb8b1abfa9fa82f2`。[EOS 首次使用记录](../../docs/eos/develop/admission-revision.md#post-merge-r03-qualification)包含本地格式、Clippy、测试、构建证据和全新主目录重放命令。重新构建合并后的主检出目录二进制文件后，真实 gRPC 运动/Get、错误角色和过期修订拒绝、不透明查询、逐角色授权轮换、旧授权拒绝与停止后退役通过；完整输出位于 IsOS 被忽略的 `logs/test/20261102-r03-cross-repo/post-merge-primary.log`。所选 EOS runtime 声明 `managed-control-grants`，IsOS P05 在停止子进程或签发授权前，于 EOS 进程 `/node_type` 要求该特性。此处仅勾选 R03；后续 M3 条目须有各自证据，限定范围的里程碑验收现已完成，见下文。

R04 所有者实现及可检查退休边界已完成。EOS 宿主基础、七方法 SkillService/Broker 与候选预检分别经 [PR #317](https://github.com/Island-Team/Island-eos/pull/317)、[PR #318](https://github.com/Island-Team/Island-eos/pull/318)、[PR #321](https://github.com/Island-Team/Island-eos/pull/321) 合并；IsOS 选择、事件策略、预检与可检查退休修复分别经 [PR #54](https://github.com/Island-Team/isos/pull/54)、[PR #55](https://github.com/Island-Team/isos/pull/55)、[PR #56](https://github.com/Island-Team/isos/pull/56)、[PR #58](https://github.com/Island-Team/isos/pull/58) 合并。[精确合并版本首次使用记录](../../docs/eos/develop/host-skills.md#exact-merged-tip-r04-checked-retirement-qualification-2026-11-02) 固定 EOS `317e4520`、IsOS `e34b7e7`、MuJoCo `c171208`，覆盖旧运行停止确认与未知、重试栅栏、保留事件及授权 token 零泄漏；IsOS [PR #59](https://github.com/Island-Team/isos/pull/59) 仅修改文档。精确版本的实时重放未覆盖完整 90 秒预检超时或预检后可执行文件／库变更；缩短期限的测试 hook 与先前受控变更运行是独立证据。R04 勾选表示经评审的所有者交付；R06 状态见下文，M3 限定范围的集成验收见下文；M5 发布验收仍待完成。

R05 已通过各所有者的独立设计与实现评审，以及限定范围的封存跨所有者资格验证。MuJoCo [PR #20](https://github.com/Island-Team/isos-mujoco-sim/pull/20) 已 squash 合并到 `master`，提交为 `c32e8fcdff2df4f99ef8279a2aa795b31046af76`；EOS [PR #319](https://github.com/Island-Team/Island-eos/pull/319) 已 squash 合并到 `refactor(v2)/runtime-main`，提交为 `97ec226b44fbb5e94b755da01a67ae82ad9f1b95`。两个合并后的 Git tree 与各自经评审的最终专题分支完全一致。EOS 主检出目录下被 Git 忽略的 `logs/test/20261102-r05-cross-owner-qualifier/` 保存 85 个通过校验和验证的文件及独立 `reviewer-verdict.md`：全新 IsOS 主目录运行了两代 pick-place 和一代 navigation，验证相机/底盘/机械臂严格健康、只读拍摄与观测、就绪事件顺序、旧授权拒绝、场景/设备/资源篡改反例、子进程/套接字/授权清理，以及持久不确定命令不重放。所有者门禁为 MuJoCo CTest 18/18 与 EOS 串行工作区测试 259 项通过、1 项已有测试忽略。实时 EOS 目录未展示 `base.velocity` 和 `arm.movej`，因为 MuJoCo 适配器尚无最终停止栅栏。R05 勾选仅涵盖严格套接字校验与就绪；后续 R06 停止与受管理运动的所有者路径见下文，后续限定范围的 M3 控制重放见下文；发布仍未完成。

R07 已通过各所有者独立设计与实现评审，以及合并后限定范围的边界资格验证。EOS [PR #323](https://github.com/Island-Team/Island-eos/pull/323) 与 [PR #324](https://github.com/Island-Team/Island-eos/pull/324) 分别以 `1d176464`、`317e4520` 合并到 `refactor(v2)/runtime-main`；IsosUI [PR #37](https://github.com/Island-Team/IsosUI/pull/37) 与 [PR #38](https://github.com/Island-Team/IsosUI/pull/38) 分别以 `5eec55e3`、`55c54a0` 合并到 `master`。合并 tree 与已评审源码一致。EOS 主检出目录中被忽略的 `logs/test/20261102-r07-async-live-replay/` 和 `logs/test/20261102-ui-stop-live-9479/` 分别有通过校验的 88 文件及 58 文件清单：受管理 mock 底盘的一次 UI 点击在 `Sending`、`InProgress`、持久化 `OutcomeUnknown` 间保留同一命令 ID，journal 仅一条记录；公开结果所有权、Origin/Host/body/grant/绑定/目标/修订拒绝、bridge 故障、旧运行不透明性、`Releasing` 期间 Stop、重启不重放以及同一运行的已确认 Stop 展示/结果围栏均通过。最终 UI 重放的已确认 Stop 和传输故障来自真实 IsOS；所有者拒绝使用精确 IsOS HTTP 422 信封通过替身检查。挂载 UI 测试覆盖未知 Stop 终态；mock 图未实时注入此结果。EOS 精确版本通过格式、Clippy、构建、330 个串行测试与样本门禁；UI 通过 113 个测试及两种构建。此勾选涵盖限定范围的 R07 浏览器路径与首个 UI 原语演示；MuJoCo 物理停止属 R06、完整逐绑定工作台属 U04、完整首次使用发布重放属 M5。

R06 所有者实现在 EOS [PR #322](https://github.com/Island-Team/Island-eos/pull/322) 以 `1204c84849f7675e9f8393488f6f0ef60ee118d7` 合并。[EOS 停止设计](../eos/design/runtime/simulator-stop_cn.md#已合并所有者与限定范围资格状态2026-11-02)记录经审查的三所有者精确提交重放及合并输入的最终负例：两场景的受治理底盘/机械臂运动后，选中管道停止得到确认；停止套接字丢失、子进程/提供者死亡及强杀归类 unknown；错误管道伪造凭据及无响应停止在管理器重启后仍为 unknown，并阻止 Restart。完整所有者日志保留在主 EOS 与 IsOS 被忽略的 `logs/test/20261102-r06-*/` 目录。证据范围有限；实时物理部分证明、Stop 进行中管理器死亡、就绪后 GUI 渲染阻塞及完整 M5 首次使用重放仍未完成。R06 按原始条目的已审查所有者验收勾选；下述更广泛实时故障及 M5 关卡仍待完成。

M3 按[精确合并版本验收记录](m3-qualification_cn.md)限定范围关闭。EOS `2437f81a`、IsOS `b5553ad9`、MuJoCo `4e6b5449` 与 IsosUI 源码 `40297289` 为固定输入；EOS 主检出目录被忽略的 `logs/test/20261102-m3-exact-merged-replay/` 保存 158 文件校验和清单及独立评审。R01–R07 所有者证据与当前提交测试共同支持原始 M3 完成条件。M4 按下述独立评审的完成记录关闭；M5 仍未完成。

Q03 结合冻结的 final-17 选定矩阵及另行固定版本的补充归属方关卡完成。EOS 的[源码和基准](../eos/maintenance/q03-evidence_cn.md)、IsOS 的[归属方验收](https://github.com/Island-Team/isos/blob/master/docs_cn/isos/design/q03-qualification_cn.md)、EGIA 的[源码／安装 CI 与五次基线](https://github.com/Island-Team/Island-egia/blob/master/docs/egia/develop/build-and-run.md#q03-owner-measurement-2026-11-03)、IsosUI 的[浏览器基线](https://github.com/Island-Team/IsosUI/blob/master/docs_cn/isosui/q03-evidence_cn.md)及 MuJoCo 的[套接字基线](https://github.com/Island-Team/isos-mujoco-sim/blob/master/docs_cn/mujoco/q03-qualification_cn.md)记录命令、环境、初次失败、吞吐／中位数／p99 实测及评审处置。final-17 安装后六案例矩阵针对[选定矩阵证据](../../docs_cn/eos/maintenance/q03-selected-matrix-evidence_cn.md#final-17-选定主题提交结果2026-11-03)中的准确主题提交通过完整离线验证；准确 EOS `219f031` 源码关卡和只读选定源码验证也通过。IsOS [PR #75](https://github.com/Island-Team/isos/pull/75) 增加选定归属方 HTTP Vision 状态生命周期测试；EOS [PR #347](https://github.com/Island-Team/Island-eos/pull/347) 和 EGIA [PR #108](https://github.com/Island-Team/Island-egia/pull/108) 封存并记录真实安装后 EGIA 托管进程的故障、重启、停止及启动前反例重放。补充测试各有自己的固定输入，不改变 final-17 或 Q04 final-5 的冻结选定。另行选定的 Q04 final-5 [合并提交归档](q04-merged-qualification_cn.md#第五次尝试merged-home-final-5完整归档已验证)已通过；Q03 后 EOS 集成分支再次合入 master、公开快照、发布评审及标签仍属 Q04 工作。

首次 Q04 合并提交重放尝试使用[双语验收记录](q04-merged-qualification_cn.md)中冻结的五个集成分支准确提交。运行 `20261103-q04-merged-final-1` 在 R06/A03 案例的 Apply 阶段返回 binary 阶段的 `generated_missing_input` 后停止。这是失败的诊断证据，并非 Q04 通过。MuJoCo 发布修复后已开始重新选定与重放；尚无发布决定。

MuJoCo 修复后的第二次合并提交尝试 `20261103-q04-merged-home-final-2` 在启动前失败：IsOS 报告 `vision_endpoint_unavailable`，检查套接字的路径为 111 字节，超过 Linux Unix 套接字路径的 107 字节上限。[验收记录](q04-merged-qualification_cn.md)保留其新冻结输入与归档。IsOS 与 EGIA 协同完成端点归属修复后进行了 final-3；Q04 保持开放。

第三次合并提交尝试 `20261103-q04-merged-home-final-3` 到达实时 UI Control `Completed`、可见 Vision 帧、重启与确认的 Stop/回收。但冻结的捕获辅助程序在短端点归属方合并后仍使用旧 Vision 套接字路径，因此缺少必需的托管/浏览器原始证据，不能封存。EOS harness [PR #343](https://github.com/Island-Team/Island-eos/pull/343) 已将路径修正合入 `refactor(v2)/runtime-main`，提交为 `f8945d7a`；下述 final-4 使用该合并修正。Final-3 是失败的验收证据，并非 Q04 关闭。

新的 final-4 合并提交重放通过了干净 home 实时主路径：可见 Control 命令完成、Vision 帧已渲染、公开 bridge 结果关联、Restart 与确认的 Stop。[验收记录](q04-merged-qualification_cn.md#第四次尝试merged-home-final-4实时通过案例验证器失败)冻结准确输入和归档。缺少 EGIA `pytest` 测试依赖的问题经评审允许仅恢复案例阶段后，R06/A03 实时执行到达选定 EOS 验证器；它错误要求冻结的 MuJoCo release 中 `required_libraries` 为空。Final-4 未取得案例收据或封存，验收失败。EOS [PR #344](https://github.com/Island-Team/Island-eos/pull/344) 已合入验证器修正；下述 final-5 使用新的选定文件。Q04 保持开放。

Final-5 的[准确合并提交六案例归档](q04-merged-qualification_cn.md#第五次尝试merged-home-final-5完整归档已验证)已通过：实时 Control `Completed`、已渲染 Vision、bridge HTTP 200、确认的 Stop 与 Restart、软件包/编辑器及边负例、日志游标、R06/A03 故障恢复和显式迁移。独立提供的选定文件、规范化外部构建收据、案例收据及封存归档通过完整离线验证（`Q01_FULL_VERIFIED`）。这验收了所选合并提交重放；EOS 集成分支合入 `master`、公开快照、发布评审与标签仍是独立的 Q04 工作。Q04 保持未勾选。
