# 浏览器原语结果路由

[English](../../../../docs/eos/design/runtime/browser-results.md) · [所有者控制契约](../../interfaces/control_cn.md#r07-浏览器授权结果后续查询) · [原语绑定](../../interfaces/primitive-bindings_cn.md)

**R07 的 EOS 与 IsosUI 限定范围实现已合并并通过资格验证。** R03 已接收来自回环 bridge 的 `primitive_typed`，并通过唯一 Dispatcher 路线准入。R07 为仍处于 `InProgress` 的命令增加授权读取，保留活动修订校验，并证明浏览器不能访问原始执行器或其他调用方的 journal 记录。IsOS 拥有保存及活动图修订和按运行签发的授权；EOS 拥有活动绑定、命令 journal 与实时结果。图边只声明绑定，绝不执行浏览器请求。

bridge 在读取私有操作员授权前，校验 Host、允许的 Origin、HTTP 请求体大小、精确操作及公开字段。它把公开 `result` 改写为私有 `primitive_result`，仅在该 owner socket 请求中附上授权，绝不返回或记录授权。它不会原样转发公开 `result`；受管理 runtime 继续拒绝历史原始 `result` 操作。runtime 核对已准入的保存字节修订，将授权映射到操作员角色。Dispatcher 增加专用只读查询：在同一 journal 锁下核对当前运行、记录中的活动修订和角色、`Movement` 类型以及准入命令记录的 `PrivateBridge` 入口，再返回 `DispatchResult`。现有 `gateway_operation_authorized` 不公开入口，单独使用不能证明浏览器绑定。错误运行、角色、种类、入口、旧身份与不存在的 ID 均返回同一不透明响应。新的运行轮换授权；即使命令 ID 仍保存在持久 journal，旧结果也不可见。精确字段、响应界限及错误由所有者契约定义。

对于 `primitive_typed`，HTTP bridge 同时是私有 IPC 的序列化方。JavaScript 和 Rust 对有限载荷数字可能产生不同字节长度。bridge 检查公开声明长度和解析后载荷均处于 16 KiB 界限内，再以自身紧凑 `serde_json` 字节填写私有 `payload_length_bytes`。公开声明是有界提示，私有请求头才是权威长度；runtime 仍检查私有字段精确匹配。这样，合法浏览器命令不会仅因 bridge 解析后把 `0.000001` 表示为 `1e-6` 而失败。不合法或过大的公开请求头在读取授权前以固定 HTTP 400 `invalid typed request` 拒绝；载荷内容和全部准入身份保持原样。

浏览器先比较 IsOS 保存修订与活动修订，取得 EOS 单调时钟，检查活动绑定的唯一目标和实时健康，再发送有界类型化请求。本地 `sending` 标记仅在 bridge 返回关联命令 ID 及 EOS 状态后结束。修订仍活动时，`InProgress` 允许有界后续查询；`Completed`、`Failed`、`Cancelled`、`Rejected`、`OutcomeUnknown` 是显示终态，其中 `Failed` 必须明确显示为终态失败。bridge 故障、修订变化或 runtime 停止使结果明确显示不可用，不能显示为完成。UI 的部署 Stop 继续通过 IsOS 可见；R06 提供物理执行器停止证明后，MuJoCo 运动才可启用。Vision 继续使用只读 WebSocket 及独立的活动修订检查。

## 受管理提交修正——目标设计

**前置实现行为。** 本异步切片之前，`primitive_typed` 处理器同步调用 `dispatch_managed_movement`：先持久化包含 `InProgress` 的 `Prepared`，等待适配器，写入终态，随后才回复。当前 CXX mock 返回 `Accepted`，Dispatcher 将其映射为终态 `OutcomeUnknown`；普通浏览器不能通过这条路径取得 `InProgress` 回复。并发 journal 查询可看到短暂状态，但 UI 在发送未决时不轮询是正确行为。现有 bridge、journal 读取器及 UI 替身测试证明了授权后续查询语义，但不满足受管理 mock 首次使用的 `InProgress` 条款。[所有者契约](../../interfaces/control_cn.md#r07-受管理浏览器提交)规定修正后的确认和停止行为，不增加线协议操作。

把**受管理浏览器** Dispatcher 调用拆为同步 `prepare` 和一个有界执行工作线程。准备步骤解析并授权同一私有信封，检查活动绑定及类型化目标，取得八个浏览器运动槽位之一，并持久化带完整受管理身份的 `Prepared`。只有完成该写入，socket 处理器才可返回 journal 中关联的 `InProgress` 快照。工作线程随后使用现有准入与释放闸门，包括截止时间和实时健康校验，持久化写入 `Releasing`，调用适配器一次，再写入终态。准备与工作线程须共享当前 Dispatcher journal 和释放闸门；新执行器不能持有第二个硬件句柄。gRPC 提交仍为同步，公开 `primitive_typed` 和 `result` 请求体不变。快速工作线程可能先完成，因此首次回复也可能是终态；受控延迟的 mock 测试夹具提供确定性的真实 `InProgress` 重放，但不增加图设置，也不声称生产延迟保证。

容量与重复 ID 处理属于准备事务。申请槽位**之前**先查找已有 ID，比较其持久化签名和当前授权；完全相同的重复请求只返回已有状态，不能再次排队或下发，身份不符则拒绝。新 ID 在持久化 `Prepared` 前取得槽位。运动通道已满时，另加一次性原子终态 journal 插入，存储完整签名及身份、`accepted:false` 与 `Rejected`：现有 `Journal::insert` 不能用于此处，因为它强制写入 `Prepared`，若在下一次写入前崩溃会恢复为 `Cancelled`。原子插入失败时返回固定的不可用错误，不下发。持久化预留后启动工作线程失败，须再次检查阶段：仅把仍为 `Prepared` 的记录持久化封闭为 `Rejected`；若 Stop 已写入 `Cancelled`，则由该结果优先且不得覆盖。必要的封闭失败则返回不可用并禁止运动。工作线程槽位直到终态持久化或代际退出才释放；分离的物理工作线程在存活期间仍持有旧 journal/适配器所有权，绝不进入新代际。队列或工作线程数不得无界增长。调用适配器或等待槽位时不得持有 journal 锁。安全停止通道独立于运动通道；运动容量耗尽不能延迟 Stop 准入。

关闭时，先进入 R06 `Closing`、注册停止写入方，并在等待普通工作线程或 journal 锁之前启动抢占式物理停止。工作线程在持久化 `Prepared` → `Releasing` 转换之前，须在 journal 锁下检查 `Closing` 和自身阶段。已通过检查的线程仍使用转换前捕获的代际/停止 epoch，Stop 后不得取得新 token。物理停止已排定后，在同一 journal 锁下把仍为 `Prepared` 的浏览器记录持久化封闭为 `Cancelled`。工作线程再次检查阶段，不得覆盖 `Cancelled` 或调用适配器。已处于 `Releasing` 的线程参与物理停止尝试，不能仅凭时间视为完成。若 MuJoCo 双部件停止得到确认，前进的 epoch 拒绝携带旧 token 的运动；若停止证明丢失或结果未知，epoch 可能没有前进，已在途的线程仍可能到达旧子进程。R06 保留未知回执，在期限内终止并回收该子进程。工作线程仅绑定旧 EOS 进程的适配器/会话及已捕获的代际，不能取得新 token 或重新连接替换子进程；下次 IsOS 运行启动独立的 EOS 进程。子进程清理不是物理停止已确认的证据。安全路径不加入阻塞的普通工作线程。分离的工作线程存活期间保留旧 journal/适配器所有权，不能污染替换代际。现有 journal 恢复将 `Prepared` 封闭为 `Cancelled`，`Releasing` 封闭为 `OutcomeUnknown`，对不确定结果禁止运动，两者均不重放。终态 journal 写入为单写入方转换，不得覆盖 Stop 或恢复写入的终态。IsOS 轮换运行后，现有授权及运行检查使旧结果不透明。

可能发生物理下发后的终态 journal 写入失败是一种独立的在线故障。锁存运动禁用；保留该工作线程槽位直到持久化封闭或代际退出，并在物理调用仍存活时保留旧 journal/适配器所有权。经授权的结果查询对此 ID 返回现有固定 `result unavailable` 错误，而非持久化但已过期的 `InProgress` 或仅在内存中的终态猜测；完全相同的重复提交也返回相同错误，绝不启动另一线程。调用返回、已确认的停止 epoch 将其阻断，或未知停止清理已回收其旧子进程后，尝试持久化封闭为 `OutcomeUnknown`。只有成功持久化，授权读取才可返回该未知终态；若写入仍失败，读取保持不可用，R06 停止回执只依据独立的物理证据。重启时，在就绪前将持久化 `Releasing` 恢复为 `OutcomeUnknown` 并保持运动禁用；若恢复不能持久化，启动失败。对首次终态写入、后备封闭和恢复写入注入失败，并使适配器回复在 Stop 后到达，以证明无虚假完成、槽位复用或重放。

释放前的终态写入失败也需要明确的在线故障状态。若排队工作线程因截止时间、健康或 `Closing` 拒绝，持久化阶段仍是 `Prepared`，而 `Rejected` 或 `Cancelled` 写入失败，则在放开 journal 锁前对该 ID 封锁 `begin_release`。保留槽位并锁存运动禁用；经授权读取与完全相同的重复提交均返回固定 `result unavailable` 错误，不能暴露持久化但已过期的 `InProgress`，也不能调度适配器。于 journal 锁下重试终态写入：Stop 已开始则选择 `Cancelled`，否则截止时间或健康失败选择 `Rejected`。首次成功的持久化终态转换获胜，释放槽位并可供读取。若代际退出前重试均失败，现有 journal 恢复在就绪前将 `Prepared` 封闭为 `Cancelled`，且不进行任何物理重放；恢复持久化失败会阻止启动。故障测试令排队的 mock 运动等待释放闸门，对截止时间、健康和 Stop 注入此类终态写入失败，断言无适配器调用、读取及重复提交不可用、容量保留，以及成功封闭或重启取消。

浏览器保持当前的单次提交、受保护读取状态机。关联的 `InProgress` 回复仅在 IsOS 新状态确认捕获的运行和活动修订后启动有界只读轮询；终态回复结束轮询。首次回复丢失时，浏览器可以使用已生成 ID 进行相同的受保护结果读取，但绝不能重新发送命令。`InProgress`、bridge 故障或过期修订期间，IsOS Stop 操作仍可使用。Stop 或重启后，UI 将最后的所有者状态和 ID 保留为旧运行不可用记录，忽略迟到回复，且不声称 Stop 将命令转换为 `Completed`。

## 历史设计与专题分支证据

以下验收清单与分支记录描述所有者合并前的状态；合并后的结果见下文。

修订后的 R07 关卡要求端到端的受管理 IsOS 保存/活动 mock 运行，并通过受控适配器延迟证明：操作员点击一次，EOS 在持久化 `Prepared` 后回复 `InProgress`，经授权的浏览器 `result` 查询在同一 ID 仍未终结时读到它，随后查询读到 EOS 持久化终态，期间不再次下发运动。验收清单还要求覆盖容量耗尽、相同 ID 并发重复、相同 ID 下身份变化、等待释放闸门时截止时间过期、释放前 Stop、`Releasing` 期间 Stop、首次回复丢失、两种 journal 阶段的重启恢复、旧运行不透明性、bridge 故障及独立 IsOS Stop 可见性。把完整命令、图、事件和 journal 证据保存至 `logs/test/<run-id>/`；关闭 R07 前重跑所有者及 UI 的格式、lint、测试、构建，并取得独立设计及实现审查。

R07 实现测试清单要求覆盖公开 `result` 到私有 `primitive_result` 的改写及直接原始 `result` 拒绝、活动绑定与精确目标、下发前和结果查询前的过期修订、畸形或过大请求体、伪造授权、禁止的 Origin/Host、同角色当前运行结果、旧运行和其他角色不透明性、gRPC 运动与旧记录/停止 ID 不透明性、不改动 journal 的 512 字节原因和 4 KiB 响应界限、包含终态 `Failed` 的全部六种既有结果状态、runtime 断线、`InProgress` 到终态的变化、Vision 修订拒绝不变，以及 UI 消费端的 Stop 可见性。owner socket 替身证明 bridge 准入，真实受管理 IsOS → EOS 进程重放证明 journal 所有权及命令关联。完整输出和可复现命令须保存到 `logs/test/<run-id>/`；[共享 R07 条目](../../../island/implementation-todo_cn.md)曾在独立设计与实现审查及集成边界通过前保持未完成。

此前经审查的设计修订运行 `python3 tests/fixtures/browser_results/check.py`，30 个唯一用例通过，包含具体公开/私有请求及有界响应；`python3 scripts/check_doc_links.py --all` 检查 314 个 Markdown 文件，无损坏的相对链接；`git diff --check` 通过。完整命令输出保存在已忽略的 `logs/test/20261102-r07-design/`。载荷长度规范化修正新增 BR31 和 BR32，当前样本共 32 项；修正后的设计已在 `f4dd6d04` 完成独立审查。

在 EOS 实现主题分支，`cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace -- --test-threads=1`、`cargo build --workspace`、`python3 tests/fixtures/browser_results/check.py` 和 `python3 scripts/check_doc_links.py --all` 均通过；工作区测试为 267 通过、零失败、一项忽略，链接检查为 314 个文件且零损坏。完整输出位于忽略的 `logs/test/20261102-r07-browser-results-implementation/`。bridge 替身测试覆盖六种状态、请求改写、重复或额外键、授权保密、超大响应和故障；真实受管理 EOS 进程测试覆盖原始路径拒绝、授权与修订拒绝及关联的授权结果；Dispatcher 测试覆盖私有入口、运行与 Stop 不透明性，以及不重放适配器的 `InProgress` 到终态转换。IsOS 与 IsosUI 集成首次使用重放和独立实现审查仍是 R07 关闭门槛。

载荷长度规范化实现运行 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace -- --test-threads=1`、`cargo build --workspace`、`python3 tests/fixtures/browser_results/check.py` 和 `python3 scripts/check_doc_links.py --all`，全部通过。工作区为 269 通过、零失败、一项忽略；样本为 32 项，链接检查覆盖 314 个 Markdown 文件且零损坏。完整输出位于忽略的 `logs/test/20261102-r07-payload-normalization/`。bridge 替身确认公开 84 字节 JavaScript 提示被改写为私有 80 字节 Rust 序列化长度，载荷与身份字段保持不变。缺失、非整数、零、超限及解析后超大输入均在使用授权或私有 IPC 前返回固定 HTTP 400；既有浏览器结果测试继续通过。独立实现增量审查及浏览器集成重放仍待完成。

在专门的异步提交分支上，私有 `primitive_typed` 处理器调用 Dispatcher 的 `submit_browser_movement`：先持久化受管理的 `Prepared` 记录，再返回 `InProgress`；最多八个工作线程通过既有释放闸门执行；容量耗尽时以单次 journal 插入保存完整的终态 `Rejected`。相同 ID 与身份返回保留的结果；包括截止时间变化在内的签名变化均遭拒绝。新的 `Prepared` 预留之前先检查类型化载荷的静态限制。工作线程保留同一个 Dispatcher 和适配器的所有权。终态写入失败时，结果读取及相同 ID 重复提交均不可用，运动禁用被锁存，槽位保留至持久化封闭或进程退出。既有 journal 恢复区分 `Prepared` 取消和 `Releasing` 不确定状态。仅测试用的 `aether-runtime/test-hooks` 功能接受 `EOS_TEST_MOCK_SUBMIT_DELAY_MS`，用于受控 CXX mock 进程样本；默认构建不读取该环境键。

此分支头上的 `cargo fmt --all -- --check`、带 `-D warnings` 的工作区 Clippy、工作区测试及构建、32 项浏览器样本和 314 文件链接检查均通过。工作区测试日志记录 285 通过、零失败、一项忽略。600 毫秒受管理 mock 进程测试证明持久化的 `InProgress` 确认、同一 ID 未终结时的授权结果读取，以及后续持久化的 `OutcomeUnknown` 终态读取，无须再次提交。Dispatcher 测试覆盖八槽容量、原子拒绝、相同 ID 与身份变化、排队后的截止时间复查、`Prepared` Stop 取消，以及终态写入故障与重启恢复。完整输出保存在已忽略的 `logs/test/20261102-r07-browser-async/`。R06 物理停止顺序、停止证明丢失后的迟到运动、完整 IsOS/IsosUI 实时重放及独立实现审查，仍须等待本分支刷新到已合并的 R06 停止所有者之后完成。

独立核心审查复现两处准入故障：同一 ID 更改截止时间曾被视为重复请求，超出限制的类型化值也可能在工作线程拒绝前取得持久化的 `InProgress`。修订为浏览器提交采用包含所提交截止时间的独立签名版本，并在预留前检查静态类型化限制。新增反例测试证明截止时间变化会被拒绝，底盘速度 1,000,000 米/秒不会产生 journal 记录。仅测试用的记录写入及工作线程启动故障覆盖排队后的过期、健康失败与 Stop、瞬时持久化重试、持续不可用读取及恢复；Stop 后迟到的 mock 回复保持 `OutcomeUnknown`。与 R06 合并后的停止证明仍是单独的未关闭关卡。

后续 Stop 竞态审查发现：排队请求的首次终态写入失败后、重试前，`Closing` 可能开始。现在重试在 journal 锁内重新检查 `Closing`：若命令仍为 `Prepared` 且 Stop 已开始，则持久化封闭为 `Cancelled`；`Releasing` 命令仍封闭为 `OutcomeUnknown`。确定性的测试在写入故障后持有浏览器槽位锁，启动 Stop，证明最终持久化状态为 `Cancelled`、只有先前命令调用一次适配器，且槽位仅在持久化封闭后释放。更新后的工作区关卡为 286 通过、零失败、一项忽略；格式、Clippy、构建、延迟受管理进程、32 项样本及 314 文件链接检查均通过。完整输出位于忽略的 `logs/test/20261102-r07-browser-async/`，文件名带 `-stop-race` 后缀。与 R06 合并后的边界仍未关闭。

EOS 集成变基以已合并的 R06 runtime-main 精确提交 `1204c848` 为浏览器结果与异步分支的基础。它保留 R06 的 `Arc<Mutex<Journal>>` 和受管理的物理停止启动顺序，依次执行 `quiesce`、物理 Stop，再取消仍处于 `Prepared` 的浏览器命令。异步工作线程保留旧 Dispatcher 和适配器；R06 在持久化 `Releasing` 前捕获运动栅栏令牌，Stop 后不重新取令牌。runtime 在终止子进程前关闭宿主技能工作线程。组合测试 `browser_worker_stalled_at_journal_cannot_delay_managed_physical_stop` 在异步浏览器释放期间持有 journal，证明放开锁前已经启动物理 Stop，随后结果持久化为 `OutcomeUnknown`。既有测试覆盖 `Prepared` 取消、`Releasing` 不确定结果、终态写入故障及旧运行结果不透明性。此集成分支上的 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace -- --test-threads=1`、`cargo build --workspace`、`cargo test -p aether-dispatcher --features test-hooks browser_worker_stalled_at_journal_cannot_delay_managed_physical_stop -- --test-threads=1`、R07 与 R06 样本检查以及 `python3 scripts/check_doc_links.py --all` 均通过。工作区测试为 328 通过、零失败、一项依赖模拟器的测试被忽略；链接检查覆盖 322 个 Markdown 文件且无损坏的相对链接。完整输出位于忽略的 `logs/test/20261102-r07-r06-integrate/`。最初并行运行的 focused 测试有六项对时序敏感的 R06 backend child 测试失败；隔离测试和串行 focused、工作区测试均通过，并行失败日志已保留。IsOS/IsosUI 集成首次使用重放和独立实现审查仍是 R07 关闭关卡。
## 合并后的限定范围资格验证（2026-11-02）

EOS [#323](https://github.com/Island-Team/Island-eos/pull/323) 与 [#324](https://github.com/Island-Team/Island-eos/pull/324) 分别以 `1d176464`、`317e4520` 合并到 `refactor(v2)/runtime-main`；IsosUI [#37](https://github.com/Island-Team/IsosUI/pull/37) 与 [#38](https://github.com/Island-Team/IsosUI/pull/38) 分别以 `5eec55e3`、`55c54a0` 合并到 `master`。最终合并 tree 与已评审及实时测试的源码 tree 相同。EOS 精确版本门禁通过格式、严格工作区 Clippy、330 个串行测试、构建、R06/R07 样本及 322 个文档链接；完整输出位于忽略目录 `logs/test/20261102-r07-postmerge-refresh/`。

独立[实时浏览器重放](../../../island/implementation-todo_cn.md)使用受管理 mock 底盘的一次点击：同一命令 ID 依次经历浏览器 `Sending`、EOS `InProgress` 与持久化 `OutcomeUnknown`，只有一条 journal 记录且浏览器没有再次提交。重放观察了受保护结果读取、Origin/Host/body/grant/绑定/目标/修订拒绝、bridge 故障、旧运行不透明性、`Releasing` 期间 Stop 与重启恢复。88 文件校验和清单及原始响应位于忽略目录 `logs/test/20261102-r07-async-live-replay/`。另一组 58 文件的 `logs/test/20261102-ui-stop-live-9479/` 重放使用最终 IsosUI bundle，确认同一运行的 IsOS Stop 横幅、即时 Invoke/结果围栏和保留的命令 ID。传输故障来自实时服务；所有者 HTTP 422 拒绝使用精确 IsOS 信封经替身重放。UI 对未知 Stop 的展示由挂载测试覆盖，本 mock 图没有真实未知执行器 Stop。MuJoCo 物理停止属于 R06，完整逐绑定工作台属于 U04。
