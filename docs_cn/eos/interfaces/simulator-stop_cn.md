# EOS 受管理模拟器停止契约

[English](../../../docs/eos/interfaces/simulator-stop.md) · [MuJoCo 绑定](mujoco_cn.md) · [Dispatcher](../design/runtime/dispatcher_cn.md) · [设计](../design/runtime/simulator-stop_cn.md) · [首次使用](../usage/mujoco_cn.md#r06-受管理停止首次使用) · [R06 任务](../../../docs/island/implementation-todo.md)

**所有者契约已实现并有限定范围的跨所有者证据；原始 R06 条目与限定范围 M3 已验收，进一步实时证明仍待完成。** EOS 拥有 Dispatcher 准入、设备适配器、直接启动的 MuJoCo 子进程及终端停止凭据。IsOS 拥有依赖进程组，从选中 EOS 子进程的 stdout 管道接收凭据。MuJoCo 通过独立、仅所有者可用的停止套接字证明底盘和机械臂分别保持。此契约不增加图字段，图连线或套接字客户均不能由此取得命令权限。

当此实现可用时，`aether-runtime --capabilities` 包含准确的 `managed-stop-v1` 功能 token。这只是供 IsOS 预检的静态兼容声明，不表示当前子进程已停止，也不表示选中运行已有 confirmed 凭据。确认仍须使用本次运行严格验证的 stdout 凭据及 EOS 成功退出。

## 停止触发与准入边界

IsOS 发出的 `SIGTERM`、经过所有者认证的私有 `shutdown`、致命后端状态变化或 EOS 正常退出，都会进入同一个幂等的受管理关闭状态。首次转换会关闭新的 gRPC 和私有运动准入，停止技能发布与 Vision 采集，将健康状态设为 `NOT_SERVING`，并在任何适配器调用前为 Dispatcher 建立关闭栅栏。第二次信号或关闭请求只加入已有状态，不能重启运动或再发起一次物理停止。`SIGKILL` 和进程崩溃无法产生凭据。独立启动的 EOS 保留当前尽力停止语义，不得声称拥有受管理凭据。

受管理的 MuJoCo 子进程须在主套接字和配套停止套接字均发布 `strict-stop-v1`，且提供相同的新鲜 32 位小写十六进制 `generation_id`；仅有 `strict-socket-v1` 足以支持 R05 只读就绪，不能允许受管理运动。EOS 在暴露 `base.velocity`、`arm.movej` 或机器人范围的 `robot.stop`、`safety.estop` 前探测配套停止套接字。MuJoCo 的 `stop` 会停止整台机器人，因此 EOS **不**为 MuJoCo 适配器发布设备范围的 `motion.stop`。其他方面有效的受管理 `motion.stop` Submit 返回 `FAILED_PRECONDITION` 与 `terminal_device_stop_unavailable`，不能悄然停止另一设备。EOS 在共享适配器会话中保存 generation 和规范十进制 `stop_epoch`，初始为 `"0"`。每次受管理运动携带在持久化 `Releasing` **之前**取得的 generation 和 epoch。MuJoCo 引擎在命令提交栅栏下比较两者。停止先推进 epoch，再保持两部分；`Releasing` 后延迟的旧运动会得到 `stop_fenced` 或 `command_interrupted`，EOS 不会悄然重发。仅在完整验证停止响应后，EOS 才更新缓存 epoch；部分响应或丢失响应使运动维持禁止。新子进程的 generation 不能继承旧 token 或命令结果。

对于声明了底盘或机械臂的受管理图，启动过程只在既有绝对启动期限内重试配套停止套接字的暂时性健康探测失败。无效证明或 generation 不一致会立即使启动失败。仅当每个声明的执行器在两个套接字上证明同一 generation 后，EOS 才报告就绪；只有相机的图可在没有停止 generation 时保持只读。若配套 lane 始终未绑定，则产生 `eos_start_failed`，不会输出就绪或准入运动。

EOS 在一个单调关闭期限内，通过配套套接字发送一次全机器人停止，并带上准确的子进程 generation。完整确认要求响应的版本和 ID 匹配、generation 相同、epoch 规范且更新、`base_stop:"stopped"`、`arm_stop:"stopped"`、`ok:true` 及 `accepted:true`。身份和 epoch 均有效的 `stop_outcome_unknown` 响应可单独记录一个已证明的 `stopped` 部分，另一部分维持 `unknown`，但不能确认整体停止。超时、无效帧、断连、子进程退出或响应缺失时，未单独证明的部分均为 unknown。`stopped` 表示 MuJoCo 引擎提交了底盘零驱动或机械臂保持，并阻止所有停止前的目标写入；它不表示残余物理速度在数学上为零。EOS 不得从适配器本地 `AtomicBool`、只读健康探测、租约到期或进程退出推断此证明。R05 就绪**之前**的 GUI/渲染故障使启动失败；就绪**之后**的渲染阻塞不能占用配套停止 lane，仍可得到双部分 confirmed 证明。若引擎本身停止响应配套 lane，结果才是 unknown。

一个运行范围的 MuJoCo 停止协调器独占配套套接字物理请求及其 `(generation_id, old_epoch, new_epoch, base_stop, arm_stop)` 结果。`robot.stop`、`safety.estop` 与内部关闭通过 Dispatcher 调用它；同时到来的公开停止 ID 加入同一个正在进行的物理请求，保留各自持久化 journal 身份，不能各发一次停止帧。EOS 在物理请求前立即设置内存中的机器人禁止并登记停止 writer。公开 `robot.stop` 和 `safety.estop` 持久保存各自停止意图及每部分 `attempting`/终态结果；内部托管关闭则持久保存独立的关闭标记，其所选原始终端回执在完整子进程清理后携带分类的各部分结果。journal 写入阻塞不能延迟配套停止操作。公开操作要求意图和各部分结果持久化；托管回执要求标记持久解除并验证物理停止和清理事实，之后才能写入原始回执并由 IsOS 核验。一个有界的物理结果映射到每条已选图路径；部分失败或持久化丢失使所有运动维持禁止，相关结果为 unknown。新的运动释放后再停止，须发新的物理请求。estop 的锁存不受正面受控停止结果影响。停止须抢占持有 Dispatcher 普通释放 permit 的运动：协调器预留安全 lane，在等待该运动适配器返回**之前**推进 MuJoCo 引擎 epoch。之后可以等待 journal 记录，但物理停止不能等待运动 permit 或普通采集/worker。当前按路径循环 `stop_part` 并先调用 `wait_drained` 的顺序不足以处理受管理 MuJoCo。

### 与急停分离的例行关闭状态

对于已就绪的受管理 MuJoCo 子进程，IsOS Stop 或 Restart 使 EOS 经 Dispatcher 执行托管关闭停止。它保留同样的准入关闭、抢占式物理协调器、双部分证明和 15 秒回执期限，但不创建或清除 `safety.estop` 锁存。真正经过认证的 `safety.estop` 始终锁存，即使它与关闭竞争。致命或无法证明的关闭仍为 unknown。独立运行和非 MuJoCo 回退停止维持拒绝优先的行为。

内部 `.control` 日志体新增 `shutdown_pending`（布尔值 / 无单位，默认 `false`）和 `shutdown_targets`（排序、唯一的规范硬件路径 / 无单位，默认空，最多 64 项）。`shutdown_pending=false` 要求目标列表为空；为 `true` 时要求目标非空且 `robot_id` 已绑定。目标必须等于记录意图时所选的运动适配器；重新打开时每条路径须属于已接纳的机器人和配置，否则在就绪前启动失败。体版本升级为 `1.1.0`。读取器先严格解析特定版本的 `1.0.0` 体并验证原有规范序列化及校验和，再规范化为假/空；`1.1.0` 体按自己的形状严格解析和验证。未知字段、版本、无效目标和校验和不符均使启动失败。下一次成功的控制状态修改以原子方式写入 `1.1.0` 体及校验和；仅仅读取不改写有效旧记录，升级保留旧有抑制和急停值。未解决标记使后续每个 EOS 进程都禁止运动；它与 `robot_inhibited`、结果不明的日志记录及 `estop_latched` 独立。图或公开协议字段不变。

EOS 必须持久设置标记，才能宣称例行物理停止成功；物理伴随停止在等待日志前启动。待处理位和目标列表在同一日志锁下以原子方式设置或解除。意图写入失败或停滞时仍须及时发起物理停止，但不得声称标记已持久化或发出确认回执；此窗口中的崩溃或退出由所选 IsOS 视为 unknown 并阻止替换。托管回执须具备双部分停止及 epoch 证明、所有已接纳公开停止 ID 完成、宿主技能和证据服务关闭、子进程组无需强杀即清空及回收、后端及控制套接字移除、期限满足，以及全部其他既有回执条件。只有这时 EOS 才能先持久栅栏终态结果不明的旧运动、保留其机器人抑制，再在一次确认的原始回执写入前解除自己的关闭标记；运动若仍处于非终态，标记维持待处理。公开入口先于停止接纳结束而关闭：关闭前接纳的认证 `safety.estop` 必须在同一期限内持久锁存并完成，否则回执为 unknown 且标记维持待处理；锁存写入失败后的终态 `Rejected` 不算托管标记解除所需的完成，即使之后标记写入成功也一样；关闭后的急停被已关闭入口拒绝。重复关闭只加入首次意图和物理结果。旧进程直至退出仍受抑制。标记解除失败、部分证明、强杀、清理缺失或停止未完成都不能形成确认的所选停止。解除标记绝不清除 `robot_inhibited` 或 `estop_latched`。持久意图之后、解除之前崩溃会留下待处理标记；解除之后、原始回执之前崩溃会让 IsOS 的依赖停止保持 unknown 并阻止替换。所选图若无运动适配器，则不能声称取得此托管运动回执。参见[托管关闭设计](../design/runtime/managed-shutdown_cn.md)。

## 选中子进程终端凭据

受管理关闭时，EOS 在准入关闭、两部分物理结果分类、独立 MuJoCo 进程组全部成员退出且 leader 回收，以及后端两个套接字与 EOS 控制套接字均删除**之后**，向 stdout 管道写入恰好一行不超过 4096 字节（包含换行符）的 UTF-8 JSON。该凭据与 P06 stderr JSONL 分离，不得从 `eos_stopped`、退出码或日志库记录重建。EOS 将 stdout fd 设为非阻塞，等待可写后执行一次不超过 `PIPE_BUF` 的写入；短写或管道不可用时不得产生 confirmed 凭据，也不得等过期限。EOS 启动 MuJoCo 和其他 helper 时明确重定向或关闭 stdout/stderr，并对无关描述符设置 close-on-exec，避免任何后代在 EOS 退出后仍持有选中 EOS stdout 的写端。IsOS supervisor 拥有该**原始选中子进程管道**：严格凭据解析器先于日志规范化读取该行，再通过专用有界通道转交。它拒绝重复 JSON 键、未知或缺失字段、超长或不完整行、第二份凭据，以及停止期限后的凭据；将内容与实际观察到的选中 EOS PID、已准入 run/修订、后端节点和执行器路径比较，`confirmed` 还要求 EOS 退出码为零。错误管道或无效凭据一律 unknown。IsOS 不接受 MuJoCo、EGIA、bridge、config 或旧 EOS 运行产生的凭据。

```json
{"schema_version":1,"type":"eos_terminal_stop","run_id":"r42","eos_pid":4201,"active_revision":"7a8daee6b7db9105f5483506ffc1743625b63089710dfbf936622329c471016b","backend_node":"eos/process/mujoco/sim","backend_pid":4210,"backend_pgid":4210,"generation_id":"00112233445566778899aabbccddeeff","stop_epoch":"1","admission_closed":true,"parts":[{"node_path":"eos/hardware/base/base-0","kind":"base","outcome":"stopped"},{"node_path":"eos/hardware/manipulator/arm-0","kind":"arm","outcome":"stopped"}],"backend_group_drained":true,"backend_reaped":true,"sockets_removed":true,"outcome":"confirmed","reason_code":null}
```

`parts` 按规范路径顺序恰好列出已选图中的可停止 MuJoCo 底盘和机械臂节点，每个一次。若场景未准入某种执行器，才可省略其条目；MuJoCo 的全机器人响应仍须报告底盘与机械臂的状态。显式 unknown 凭据使用 `outcome:"unknown"`、固定的 `reason_code`，并保留已单独证明的部分状态，不能以空 `parts` 隐藏已选执行器。

| 字段 | 类型 / 单位 | 用途、缺省、校验与兼容性 |
| --- | --- | --- |
| `schema_version`, `type` | 无符号整数和字面量 / — | 必须分别为 `1` 和 `eos_terminal_stop`；未知版本、重复或额外字段均拒绝。这不是 P06 事件。 |
| `run_id` | ASCII token / — | 必须等于受管理 IsOS 运行 ID，长度 1–64，仅字母、数字、`_` 或 `-`；无缺省。 |
| `eos_pid`, `backend_pid`, `backend_pgid` | 正无符号整数或可空后端 ID / 进程 ID | 必须是观察到的 EOS PID、其直接 MuJoCo 子进程 PID 和独立进程组 ID（`setsid` 后 `backend_pgid == backend_pid`）；IsOS 将 EOS PID 与选中且尚未回收的子进程比较，不能取自日志。仅在启动前回滚且结果 unknown 时，缺失的后端 ID 可为 null。 |
| `active_revision` | 64 位小写十六进制字符串 / SHA-256 | 必须是 EOS 准入的 IsOS 已应用修订；不可退回到图字节或调用者输入。 |
| `backend_node` | 规范短图路径 / — | 必须是所选 `eos.process.mujoco@v0.1` 实例路径，例如 `eos/process/mujoco/sim`，最多 512 字节；IsOS 对照准入选择。旧式 `/cores/...` 指针在此字段无效。 |
| `generation_id` | 32 位小写十六进制字符串或 null / 子进程身份 | 主套接字与停止套接字证明的当前 MuJoCo generation；仅在子进程未就绪且结果 unknown 时可为 null。双套接字证明就绪后，即使停止套接字消失或后续停止响应丢失，EOS 也在终止凭据中保留这个已验证身份；它不是授权秘密。 |
| `stop_epoch` | 规范 ASCII 十进制字符串或 null / 子进程本地计数器 | 分类后的停止返回 epoch；无有效响应时为 null。confirmed 必须提供且大于停止前捕获的 epoch；它不是图修订。 |
| `admission_closed`, `backend_group_drained`, `backend_reaped`, `sockets_removed` | 布尔值 / — | EOS 关闭状态及自有资源的准确事实；confirmed 时均必须为 true。`backend_group_drained` 表示回收 leader 前，其原 PGID/session 和已证实的后代中除被持有的 leader 外已没有存活或 zombie 成员，也没有归属不明的新收养 EOS 直属子进程。不能从 leader PID 消失或提前看不到路径推断。 |
| `parts` | 数组 / 记录 | 必须是已选执行器，最多 64 项，按规范路径排序且路径和 kind 唯一；无缺省。每项仅有 `node_path`、`kind`（`base` 或 `arm`）和 `outcome`（`stopped` 或 `unknown`）。 |
| `outcome` | `confirmed` 或 `unknown` / — | 只有全部已选部分 stopped、MuJoCo 双部分证明完整、准入关闭、子进程组排空且 leader 无需强杀即被回收、套接字删除且在期限前写入凭据，才能 confirmed。unknown 是该运行的终态。 |
| `reason_code` | 固定代码或 null / — | confirmed 时且仅此时为 null；unknown 使用 `stop_timeout`、`stop_socket_unavailable`、`stop_response_invalid`、`stop_part_unknown`、`backend_exited`、`backend_forced_kill`、`backend_group_unreaped`、`backend_reap_failed` 或 `socket_cleanup_failed`；不含原始路径、错误或秘密。 |

机械臂保持失败的 unknown 示例：

```json
{"schema_version":1,"type":"eos_terminal_stop","run_id":"r42","eos_pid":4201,"active_revision":"7a8daee6b7db9105f5483506ffc1743625b63089710dfbf936622329c471016b","backend_node":"eos/process/mujoco/sim","backend_pid":4210,"backend_pgid":4210,"generation_id":"00112233445566778899aabbccddeeff","stop_epoch":"2","admission_closed":true,"parts":[{"node_path":"eos/hardware/base/base-0","kind":"base","outcome":"stopped"},{"node_path":"eos/hardware/manipulator/arm-0","kind":"arm","outcome":"unknown"}],"backend_group_drained":true,"backend_reaped":true,"sockets_removed":true,"outcome":"unknown","reason_code":"stop_part_unknown"}
```

## 期限与生命周期

EOS 从首次停止转换 `t0` 开始计算单一单调 15,000 ms 关闭期限：`t0+1,000 ms` 前关闭入口并快照目标，`t0+4,000 ms` 前分类停止套接字结果，`t0+10,000 ms` 前终止并回收子进程，`t0+13,000 ms` 前删除套接字并发布凭据，`t0+15,000 ms` 前退出。这些是总期限内的硬性**阶段期限**；错过任一阶段就将终态设为 unknown 并进入下一清理阶段，不延长时间。准入关闭后物理停止最多 3,000 ms；子进程 TERM/回收最多 6,000 ms，其中必要的身份安全 KILL 前最多等待 3,000 ms；清理及非阻塞凭据最多 3,000 ms。安全路径不会等待被阻塞的普通 worker。强杀、无法回收或清理失败，即使已获得正面物理响应也为 unknown。IsOS 默认的 20,000 ms EOS 宽限为管道交付及其自身终态发布留出 5,000 ms；显式配置更短宽限可能得到 unknown，绝不能当作成功。IsOS 的 30,000 ms 部署期限及最后 2,000 ms 强制清理仍是外层限制。无论 GUI、相机、套接字或 journal 是否阻塞，EOS 都须在自身 15,000 ms 内完成或归类 unknown。

EOS 用当前 correlation ID 发出 `eos_stopping`、必要时的 `eos_stop_unconfirmed` 及 `eos_stopped` 诊断事件；只有选中管道凭据加成功 EOS 退出才可确认 IsOS 所有者结果。凭据前的崩溃或强杀导致 unknown；普通重复停止只加入首次停止。结果不确定的运动在 EOS 重启后仍受 journal 禁止；启动、停止或恢复路径都不能自动重发可能已释放的运动。即使 IsOS 曾在运行中返回 `confirmed`，manager 重启后仍降为 `unknown`，因为 IsOS 有意不持久化 confirmed；只有经审查的跨所有者恢复栅栏证明上一运行安全，才可启动新 Run。替代运行还须使用新的子进程 generation；历史凭据或日志不能悄然解除阻止。

**相关：** [IsOS 依赖停止](https://github.com/Island-Team/isos/blob/master/docs_cn/isos/interfaces/dependent-stop_cn.md) · [MuJoCo 停止套接字](https://github.com/Island-Team/isos-mujoco-sim/blob/master/docs_cn/mujoco/interfaces/socket_cn.md)
