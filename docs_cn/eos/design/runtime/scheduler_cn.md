# 已路由连接的调度

[English](../../../../docs/eos/design/runtime/scheduler.md) · [图契约](https://github.com/Island-Team/isos/blob/master/docs_cn/isos/interfaces/node-graph_cn.md) · [运行时基础](foundation_cn.md)

**R01 已实现。** 图连线指定一个生产者输出及消费者输入。IsOS 拥有已保存的 `priority`、`frequency_hz` 字节；两个端点准入后，EOS 拥有传递。源端口 `expected_rate_hz` 只作提示，绝不自动限速。[硬件接口](../../interfaces/hardware_cn.md#r01-eos-本地证据连线的实时路由目标契约)定义目标状态字段及界限。

## 准入与所有权

G06 校验规范路径、精确类型／端口／单位／传输身份、基数、引用和环；IsOS 校验整图，EOS 重验自身投影。R01 当前唯一的实时组合是 `eos.hardware.camera@v0.1.frame` → `eos.interface.camera_gate@v0.1.frame`（`camera_frame`、图像、进程内）。实现沿用现有相机采集与 gate 消费者。其他已准入 EOS 连线仍显示为 `not_routed` 或 `fixed_not_scheduled`；EGIA 和桥连线不在 EOS 表中。相似的数据类别或类型后缀不能授权路由。固定安全、执行器、结果、健康和停止路径绝不进入调度器、接受用户 QoS 或建立第二条 Dispatcher 准入路径。

## 传递算法

可选有限正数 `frequency_hz` 从一个传递令牌开始。消耗后须经过正数的单调纳秒才可补充：若 `rate <= 1e9`，比较 `(elapsed_ns as f64) * rate >= 1e9`；`u64` 最大经过时间下乘积仍小于 `1.85e28`。若 `rate > 1e9`，到下一个不同的纳秒刻度才可补充一个。不得计算 `1 / rate` 或将周期转换为整数时长：G06 允许正次正规数和最大有限 `f64`。时钟向后观测不移动上次消耗时间戳。每次消费者尝试（包括失败）都消耗令牌，并以尝试时间重置消耗时间戳；最多存一个令牌，停顿后不突发。缺失字段表示无连线级限速。排队项无令牌时访问增加 `cap_wait_count`，不增加 `gap_count`。生产者自身采样上限和适配器安全边界仍独立生效；只聚合诊断，不逐样本记录。

就绪路线按字节序路由键加权亏额轮询。一个**服务片段**最多访问 64 条不同路线并执行 64 次传递尝试；下一片段从上次访问后方的持久游标继续，预算耗尽不得回到首键。完整一轮是每条就绪路线各访问一次，可以跨多个片段。nice 式 `priority` 在 `[-20,19]` 中，正权重为 `20 - priority`（`40..1`）。访问时给就绪路线增加 `weight` 单位信用，上限 1024；每单位为 64 KiB 序列化载荷，每次传递成本为 `max(1,ceil(bytes/65536))`，上限 1024。只要仍有信用和片段尝试预算，该路线可连续尝试多项；访问后即使有剩余信用或预算已尽，游标仍前进。未用信用保留到下轮，空队列清除信用。无令牌路线访问一次并增加一次 `cap_wait_count`，不消耗信用，等待下轮。65 或 256 条持续就绪、权重为一且成本一的路线分别在两个或四个片段内全部获得访问；任意正权重下，每个持续就绪路线最多 256 个片段便获得一次访问，因为每个非空片段至少前进一条不同路线。权重为一且载荷最大的路线最多访问 1024 次后积累足够信用；高权重路线可执行更多尝试。图优先级不改变操作系统线程优先级，也不保证实时性。合成 state/event 测试公平性，但不注册新图路线。

图像与观测每路线仅保留最新一项；新项替换旧项时增加 `replaced_count` 和 `gap_count`。状态和事件 FIFO 每路线最多 64 项、序列化载荷最多 1 MiB。溢出（包括单项超限）拒绝最新项，保持原顺序，并增加 `overflow_count` 与 `gap_count`。全局最多 256 条实时路线及 64 MiB 排队载荷；路线数量超限在工作线程启动前拒绝，全局字节压力拒绝最新候选项并计入溢出／间断，同时保留先前保留的最新项。替换前按新旧载荷净字节变化检查。消费者错误移除证据项，增加 `consumer_error_count` 和 `gap_count`，不声称已传递或自动重试。计数在 `u64::MAX` 饱和，只在新运行时代重置。命令和 opaque 数据绝不入队。

R01 停止首先原子地关闭证据准入并阻止执行中项迟到发布。在清理可能很大的队列或等待任何证据线程之前，运行时调用 Dispatcher `emergency_stop` 停止已准入执行器，并记录停止故障；然后取消排队项。每条路线至多有一个执行中的消费者接受；该操作携带取消令牌，必须在 100 ms 内完成或超期。取消前接受计入传递；取消或超期只计一次取消与间断。代数隔离阻止取消后迟到的发布，包括本会出现在 `VisionService::capture` 中的帧。`queue_depth` 不含执行中项。证据线程加入期限为 1 s；超期是可见停止故障，不能报告为完成。R01 测试此本地顺序和计数。R06 验证外层模拟器停止、终止及回收；R01 本身不证明物理安全停止。

100 ms 接受期限适用于使用可取消槽锁的 gate 消费者。相机适配器采集是独立的同步生产者调用，可能阻塞；停机阻止其迟到发布，证据线程最多等待 1 s，若仍阻塞则以失败退出。Rust 无法杀死该阻塞线程，也不能声称它已经加入。现有适配器宿主超时及 R06 模拟器停止／回收仍属独立的所有者边界。

Gate 消费者先在不修改可见帧槽的情况下准备帧。最终期限与取消检查在调度器锁内完成；同一把锁也用于停机封闭准入，帧槽、传递代数、计数和接受时间戳在该决定中一并提交。停机先封闭调度器，再调用 Dispatcher `emergency_stop`，之后才清理队列；暂停的生产者在封闭后不能再入队。相机采样对每个已准入的有限正数 `max_rate_hz`（包括次正规数）计算饱和 `Duration`，等待时每 100 ms 检查停止。

## VisionService 集成位置

`crates/aether-runtime/src/bin/aether-runtime/vision.rs` 保留每个相机来源一个采集线程。已准入路线表位于采集与各 `@v0.1` gate 槽之间。它在调度器锁外对帧序列化一次，再让精确声明的各条 gate 路线共享同一个不可变 `Arc<CameraFrame>`，各自限速和排队。锁内分发仅复制 `Arc` 句柄，包括 64 MiB 配额拒绝后续路线时。只有路线消费者成功接受后，才写入该 gate 专属的最新帧槽并增加该 gate 的已传递代数。R01 gate 的 `VisionService::capture` 只读取该槽和代数，不能通过浏览器预览看到等待、替换、丢弃或消费者失败的帧。共享一个相机的两个 gate 可以拥有不同限速及代数，同时不会重复采集。Gate 健康仍来自 Dispatcher 状态与健康，不来自路线计数。

现有 `@v1.0` gate 的直接预览路径作为迁移前的旧版兼容行为保留。它不是 R01 图路线，不接受用户 QoS；仅当该边两端均属于 EOS 时，R01 状态才显示 `not_routed`。新 `@v0.1` gate 不得使用该直接路径。`aether-runtime.rs` 当前仅所有者可访问的 Unix 控制套接字处理器从实时路线表构建 `route_status` 快照，不能从 `VisionService::active_gates` 或日志推断传递。状态只包含来源及目标均属于 EOS 的边；尤其须排除已准入的旧版 EGIA guard → EOS Dispatcher 跨 core `DesignOnly` 校验边，它由 IsOS 呈现。开始停机时套接字关闭或拒绝 `route_status`，不承诺 `stopped` 行。停止按上文的有界证据与 Dispatcher 顺序执行；R06 负责后续模拟器顺序。

## 诊断与验收

仅限所有者的运行时套接字目标 `route_status` 按键顺序返回已验证 EOS 本地连线声明，不包含载荷。实时路线暴露 QoS、队列深度、传递／等待／丢失计数及最后成功的单调传递时间；本地仅供设计及固定路线的调度字段为 null、`routed:false`，不推断健康；跨 core 声明不返回。IsOS 仍是部署状态权威。测试覆盖精确路线、相似但未路由的类型、假时钟限速（包括极端有限值）和时钟后退、65／256 路线游标进度、最新项替换、FIFO／全局饱和、消费者错误、有界执行中停止与紧急停止顺序，以及连线不能产生第二条执行器路径。[首次使用](../../usage/routed-edges_cn.md)与[机器夹具](../../../../tests/fixtures/routed_edges/README.md)说明已实现行为与仍需资格验证的工作。

G06 profile 准入基线可在 EOS 仓库根目录运行 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace` 和 `cargo build --workspace` 复现。2026-11-01 的完整输出保存在本地 `logs/test/20261101-g06-edge-contract/{fmt,clippy,tests,build}.log`。profile 连线夹具测试已通过；R01 传递和队列行为仍需单独的集成证据。

R01 修订后的文档与夹具检查于 2026-11-01 通过：`python3 tests/fixtures/routed_edges/check_cases.py`（28 个目标用例）、`python3 scripts/check_doc_links.py --all`（302 个 Markdown 文件，零个损坏的相对链接）、`python3 -m py_compile tests/fixtures/routed_edges/check_cases.py` 及 `git diff --check`。完整输出在本地 `logs/test/20261101-r01-review-fixes/`。这些检查只验证契约和夹具；实时传递仍须实现及集成测试。

## R01 实现验证

2026-11-01 的 R01 实现运行通过 `cargo fmt --all -- --check`、带 `-D warnings` 的工作区 Clippy、`cargo test --workspace`、`cargo build --workspace`、28 用例夹具检查、302 个 Markdown 文件的相对链接检查及 `git diff --check`。完整命令输出保存在本地忽略目录 `logs/test/20261101-r01-routed-implementation/`，`commands.txt` 列出精确命令。进程测试覆盖两个 gate 各自的限速和代数、无实时路线的旧版直接预览、跨 core 状态排除、固定路径状态行、摘要身份、无效请求及客户端停滞时的停机。调度器测试覆盖有界队列、65／256 路线游标、混合权重、极端时钟限速、错误、取消及采集线程阻塞时 1 s 加入失败。这些是 EOS 本地证据；R06 仍负责模拟器停止及回收资格验证。

2026-11-01 重新测试了独立 R01 评审的修正。Gate 帧槽仅在锁内最终期限／取消决定后发布；`begin_shutdown` 在 Dispatcher 停止前使用同一把锁封闭准入，停止后才清理队列。`last_delivery_monotonic_ns` 在成功提交时采样。正次正规数相机采样等待时长会饱和，等待期间仍可检查停机。确定性测试让消费者及生产者跨封闭点暂停、使准备超过 100 ms 期限、校验较晚的提交时钟，并用次正规数相机速率启动／停止进程。相同的完整格式、工作区 Clippy／测试／构建、夹具、链接及 diff 检查均通过；完整输出位于本地 `logs/test/20261101-r01-review-corrections/`。

后续 R01 评审发现生产者在调度器锁内复制完整帧。现在生产者在锁外序列化一次，随后分发共享的不可变 `Arc<CameraFrame>` 句柄。确定性回归测试使用 256 条路线、8 MiB 声明载荷和饱和的 64 MiB 配额，同时让停止封闭请求等待生产者锁；测试核对八个保留项的指针、248 次溢出和有界锁交接。完整格式、工作区 Clippy／测试／构建、28 用例夹具、302 篇文档链接及 diff 检查再次通过。完整输出位于本地 `logs/test/20261101-r01-shared-fanout/`。
