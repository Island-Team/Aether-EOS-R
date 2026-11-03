# R06 EOS 模拟器停止设计

[English](../../../../docs/eos/design/runtime/simulator-stop.md) · [所有者契约](../../interfaces/simulator-stop_cn.md) · [硬件](hardware_cn.md) · [Dispatcher](dispatcher_cn.md) · [进程监管](process_cn.md) · [固定用例](../../../../tests/fixtures/simulator_stop/README_cn.md)

**EOS 所有者实现已合并，限定范围的跨所有者重放已通过；原始 R06 条目与限定范围 M3 已验收。** R06 关闭 R05 严格套接字留下的安全缺口。在此候选实现之前，`StopGuard::stop` 调用 `Dispatcher::emergency_stop`，但没有物理终态凭据；`BackendChild::Drop` 曾对进程组发送 TERM，等待两秒，再发送 KILL 并回收；`Endpoint::stop` 曾仅设置本地标志并使用普通套接字；`begin_release` 在 `Releasing` 后先释放 journal 锁，再调用适配器。这些事实均不能证明已准入运动在子进程终止前被栅栏阻断。[所有者契约](../../interfaces/simulator-stop_cn.md)定义凭据和期限；本页说明实现路径。

## 单一关闭状态与释放栅栏

使用由信号处理、私有所有者关闭、后端丢失检测和正常退出共同维护的运行范围 `Running → Closing → Stopping → Terminal` 状态。首次进入 `Closing` 不可逆；它关闭 gRPC 与私有运动入口，使 Dispatcher 进入 quiesce，封闭技能及证据发布，并快照所有已准入底盘和机械臂路径。再次停止只加入已有状态。关闭协调器在任何阻塞工作前启动绝对单调期限；子进程、套接字与 worker 操作只获得剩余时间。在安排保留的停止请求之前，它不会等待普通采集、运动、技能、日志或客户 worker 完成。

Dispatcher 释放栅栏需要证明两件事：`Closing` 后新命令不能越过边界；已经持久化 `Releasing` 的命令，也不能在物理停止后写入 MuJoCo 引擎。后者要求在持久化 `Releasing` **之前**捕获共享适配器会话的 `(generation_id, stop_epoch)`，并绑定到该命令。不能在 journal 更新或排队等待之后取新的 epoch。MuJoCo 引擎在短暂的运动提交锁下校验这对值；停止 lane 在同一锁下推进 epoch 并覆盖两部分目标。延迟的旧命令得到 fenced 或 interrupted 结果。Dispatcher 将其记在原命令 ID 下，不重试；丢失结果维持 `OutcomeUnknown` 及 journal 运动禁止。停止栅栏之后到达的适配器响应不能将终态 unknown/cancelled 覆盖为 `Completed`；journal 终态写入须比较捕获的停止代际。物理 epoch 栅栏还须覆盖当前绕过优先写入释放门的 safety estop 路径。

MuJoCo 配套停止操作覆盖整台机器人。一个 `ManagedMujocoStopCoordinator` 属于子进程 generation，而非任一设备 `Endpoint`。`robot.stop`、`safety.estop` 与内部关闭均通过 Dispatcher 汇聚到它。协调器注册停止写者以阻止后续运动进入，但在物理请求前**不**调用 `wait_drained`：已准入运动可能持有该 permit 并在普通套接字阻塞。它先预留配套安全 lane、推进引擎 epoch 并取得一份底盘/机械臂结果，再等待普通适配器返回。同时到来的停止 ID 加入该物理结果；它们保留各自 journal ID 和签名，但不能重复发送物理帧。经认证的停止先登记 writer 并启动按停止 ID 键控的物理 worker，再等待 journal 互斥锁。若意图写入失败或阻塞超过物理期限，物理调用仍执行，而操作与所有者凭据保持 unknown。每份停止 journal 记录在释放前将已选目标标为 `attempting`；一个物理响应按图路径映射，并在每份加入的记录中一次更新持久化。不按路径依次调用。部分失败或终态持久化失败都保持运动禁止。新的运动释放后再次停止需要新的物理请求。受控 `robot.stop` 只有在双部分证明及持久化 journal 终态均成功后，才可发布用于后续运动的新 epoch；它不能解除 estop 锁存。关闭始终不清除准入栅栏。设备范围的公开 `motion.stop` 契约不能由全机器人请求实现，因此 MuJoCo 省略该 capability；有效 Submit 使用 `FAILED_PRECONDITION/terminal_device_stop_unavailable` 拒绝。只有主和配套套接字健康探测以同一 generation 证明 `strict-stop-v1`，MuJoCo 底盘/机械臂的 `has_terminal_stop_fence` 才为 true；该标志本身不代表可以发布 `motion.stop`。相机维持只读。超时使 EOS 视角的共享 epoch 不前进、运动禁止并报告 unknown；子进程丢失使 generation 失效。

## 物理停止与子进程生命周期

子进程仍使用已验证包、场景和图绑定启动；R06 还要校验 `<主套接字>.stop` 符合 Unix 路径长度并仅所有者可访问。EOS 就绪前，在主套接字与配套停止套接字上探测所有已声明执行器。R05 相机就绪证明**之前**的 GUI/渲染故障使启动失败；停止 lane 不可用时，受管理运动及机器人范围的终态停止保持隐藏。就绪**之后**的渲染 worker 阻塞不能占用配套停止 lane，只要引擎应答，双部分停止仍可 confirmed。运行中停止 lane 健康丢失会使执行器可用性退化并阻断新的释放。即使普通请求阻塞，关闭协调器也通过配套套接字发送一次全机器人停止。它验证版本、ID、generation、新 epoch 和底盘/机械臂状态，再映射至所选图路径。MuJoCo 契约保证成功连接后分类最多 500 ms，有效帧后执行最多 1000 ms；EOS 的 3000 ms 物理停止预算还覆盖连接、写入、读取与本地分类。超时或畸形部分响应不能成为确认。

只有取得停止证明或明确归类 unknown 后，EOS 才可终止 MuJoCo。[R04 宿主 worker broker](skills/host-rust_cn.md) 在 Dispatcher 停止准入后关闭入口；物理停止先于等待宿主 worker 执行，有界 worker 关闭须在 MuJoCo 仍存活时完成或明确失败。宿主 worker 关闭失败使受管理终端凭据为 unknown。MuJoCo 在独立 session 启动，仅回收 leader 不能证明该进程组后代均退出。只要仍可能对锁定 PGID 发信号，leader 须保持未回收，以防 PGID 被复用。须将当前可能提前回收 leader 的 `try_wait` 存活轮询改为 `waitid(..., WNOWAIT)`。EOS 在 spawn 时记录 leader 的 PID、启动 ticks、PGID 与 session；TERM 或 KILL 前将 `/proc/<pid>/stat` 与这些值比较。它向这个被持有的组发送 TERM，以不回收方式观察 leader，并扫描 `/proc`，直至锁定 PGID/session 不再有其他成员；扫描失败或期限内仍有成员均为 unknown。EOS 作为 child subreaper 检测脱离进程组后被收养的后代；任何脱离或无法核实的存活成员都是 unknown。不能因为 leader 已退出或 stdout 达到 EOF 就忽略后代。存活成员在阶段期限触发身份安全的组 KILL，之后有界轮询排空/回收，但强杀即使成功仍为 unknown。只有进程组排空、不再需要对组发信号，EOS 才回收 leader 并报告 `backend_group_drained:true,backend_reaped:true`；回收 leader 后绝不再发送 KILL。启动时还给子进程设置 `PR_SET_PDEATHSIG=SIGTERM`，并在 `setsid` 后再次检查父 PID，防止 EOS 崩溃后有意受管理的模拟器无人看护；该信号只是清理，不是物理停止证明，IsOS 仍报告 unknown。EOS 明确重定向每个子进程/helper 的标准流，对无关描述符设置 close-on-exec，防止后代保持选中 EOS stdout 凭据管道打开。发布 confirmed 凭据前，明确删除后端主、配套和私有控制套接字；`Drop` 可以清理 unknown 路径，但不能升级结果。管道关闭后，被卡住的日志 worker 不得拖延安全期限。最终分类后才发布凭据，随后执行一次有界 stdout 写入并退出；P06 stderr 事件只是诊断，不能代替选中管道凭据。

R06 自 EOS 首次停止转换 `t0` 起限时 15 秒：`t0+1s` 前关闭准入，`t0+4s` 前分类物理停止，`t0+10s` 前完成 TERM/KILL/回收，`t0+13s` 前清理套接字并写凭据，`t0+15s` 前退出。协调器用定时器唤醒，不能只依赖普通 accept 循环察觉信号。任一阶段超时都变成 unknown 并进入清理；任何阶段不能延长总期限。IsOS 默认在其 30 秒部署停止期限内给 EOS 20 秒宽限，并保留最后 2 秒进行强制组清理。配置的 EOS 宽限若短于 EOS 上限，是显式所有者选择，可能返回 unknown；任何一方都不能将超时停止称为 confirmed。只有 confirmed 凭据与干净终止时，EOS 退出码才是零。unknown 凭据使用非零退出码。凭据使用一次不超过 `PIPE_BUF` 的非阻塞 stdout 写入；管道满或关闭时 IsOS 得不到可用凭据并判为 unknown。

## 测试与证据门槛

归档的 R06/A03 故障用例仅按 [MuJoCo 绑定契约](../../interfaces/mujoco_cn.md)规定的空字段省略规则比较所选发布清单与 IsOS 安装视图。先用安装收据验证安装发布清单原始字节的 SHA，再比较其余全部字段。当前发布清单的非空 SONAME 列表和 `tested_ubuntu` 必须完整保留；即使重封存归档，变更列表或补入非空 `data_types` 也必须被语义检查拒绝。

Q04 验证器修正使用选中的 final-4 R06/A03 归档运行 `python3 tests/verify_q01_r06_a03.py <archive> --expect-selection <frozen-selection> --expect-selection-sha256 <frozen-sha>`，结果为 `Q01_R06_A03_ARCHIVE_PASS`。限定测试以同一冻结输入设置 `PYTHONPATH=tests`、`Q01_R06_ARCHIVE`、`Q01_R06_SELECTION` 和 `Q01_R06_SELECTION_SHA256`，再执行 `python3 -m unittest tests/test_q01_r06_release_views.py -v`；六项测试通过，包括重新封存后的语义篡改及独立的错误 SHA 拒绝。`python3 -m compileall`、`cargo fmt --all -- --check`、带 `-D warnings` 的工作区 Clippy 和 `python3 scripts/check_doc_links.py --all` 均通过。完整输出位于被忽略的 `logs/test/20261103-q04-release-view-verifier/`。此限定验证器修正本身不关闭 Q04。

单元测试使用假时钟、暂停的 `Releasing` 钩子及模拟 MuJoCo 停止响应，证明停止抢占活跃运动 permit、旧 epoch 运动之后不能到达引擎、一份全机器人物理结果映射至所有 journal 部分、同时发生的 robot/estop/shutdown 只加入一次请求、设备范围的 `motion.stop` 保持隐藏，以及迟到的适配器结果不能清除 unknown journal 禁止。替身进程测试占满普通入口、采集槽及两个停止入口槽，再验证配套 lane 分类界限、超时及部分证明。后代替身在 MuJoCo leader 退出后仍存活；EOS 必须升级停止、归类 unknown，不能将回收 leader 当作进程组排空。真实子进程测试覆盖 `pick-place` 和 `navigation`，检查选中**原始** stdout 凭据与退出码，确认进程组排空、回收及套接字清理。注入子进程崩溃、就绪前 GUI 故障、就绪后渲染阻塞、错误 generation、旧 epoch、畸形响应、停止套接字丢失、继承的 stdout fd、满管道、TERM 超时、强杀与重复 Stop。与 IsOS 的跨所有者重放证明依赖进程逆序停止，以及缺失、伪造、重复键、错误后端、错误部分或错误 run 的凭据均为 unknown；配置的 EOS 宽限短于 15 秒时，迟到凭据不能 confirmed。完整命令输出须保存在 `logs/test/<run-id>/`；仅有静态夹具检查不能证明安全。

设计清单在 [`cases.json`](../../../../tests/fixtures/simulator_stop/cases.json) 中固定 59 项。此设计分支上执行 `python3 tests/fixtures/simulator_stop/check.py`、`python3 scripts/check_doc_links.py --all`、JSON 示例解析及 `git diff --cached --check` 均通过；完整输出位于 Git 忽略的 `logs/test/20261102-r06-eos-design/`。这仅是设计证据。独立设计审查、实现、物理集成与首次使用重放仍须完成，R06 才能勾选。

对设计提交 `2ac7a314` 的独立审查指出四项阻断：现有 Dispatcher 停止按路径循环且等待释放门、未证明后代进程组排空、选中管道解析器规定不足且后端路径用了错误的长格式，以及混淆就绪前 GUI 故障和就绪后渲染阻塞。本次修订规定单次可抢占的物理协调器与 journal 映射、持有 leader 的进程组排空、原始管道及继承 fd 规则、规范短路径和分离故障用例。`python3 tests/fixtures/simulator_stop/check.py`、`python3 scripts/check_doc_links.py --all`、`git diff --cached --check` 和 `python3 -m py_compile tests/fixtures/simulator_stop/check.py` 均通过；修订后的完整输出位于 Git 忽略的 `logs/test/20261102-r06-eos-design-review-fix/`。实现前仍须复审。

## 实现候选与本地证据

候选实现在持久化运动释放前取得 MuJoCo generation/epoch token，可在普通运动阻塞时通过配套 lane 发起一次全机器人停止，将底盘和机械臂结果映射到持久化停止部分，不发布设备范围的 `motion.stop`，并在子进程组扫描、回收及套接字清理后发出选中终端凭据。`aether-runtime --capabilities` 的 `managed-stop-v1` 只是静态兼容声明，不能替代选中凭据；进程测试检查该准确探测。停止 wire 测试检查请求加入、新运动释放后再次物理停止、部分证明维持 unknown，以及旧 epoch 运动序列化。Dispatcher 测试检查抢占、迟到运动维持禁止，以及 journal 意图容量失败时仍调用物理停止。子进程测试检查 leader 退出后的幸存组成员、脱离原 session 后被收养的后代，以及强杀维持 unknown。子进程在 TERM 前已退出时，凭据也不能确认。凭据单元测试要求双部分证明与进程组回收。

真实就绪子进程的停止套接字丢失重放发现凭据身份缺口：物理停止证明缺失时，虽然两个套接字已证明所选子进程 generation，`generation_id` 仍为 null。EOS 现在在 `BackendChild` 中保存已验证的就绪 generation，优先使用可用的会话 token，并在 unknown 终止结果中输出该已验证身份。没有有效响应时 `stop_epoch` 仍为 null；不同 generation 的证明不能确认结果或提供 epoch。针对性凭据回归覆盖响应丢失、就绪前 null 及证明不匹配。2026-11-02 执行 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、串行 `cargo test --workspace --no-fail-fast -- --test-threads=1`、`cargo build --workspace`、R06 固定用例检查及全部文档链接检查，均通过。完整输出保存在被忽略的 `logs/test/20261102-r06-generation-receipt/`。集成此凭据修复前仍须独立差异审查和真实跨所有者重放。

2026-11-02 执行 `/home/ashex/.cargo/bin/cargo fmt --all -- --check`、`/home/ashex/.cargo/bin/cargo clippy --workspace --all-targets -- -D warnings`、`/home/ashex/.cargo/bin/cargo test --workspace --no-fail-fast -- --test-threads=1`、`/home/ashex/.cargo/bin/cargo test -p aether-dispatcher --features test-hooks --test gateway_stop managed_whole_robot_stop_preempts_blocked_release_and_fences_late_completion -- --nocapture`、`/home/ashex/.cargo/bin/cargo build --workspace`、`python3 tests/fixtures/simulator_stop/check.py` 及 `python3 scripts/check_doc_links.py --all`，全部通过；完整输出保存在被忽略的 `logs/test/20261102-r06-simulator-stop/`。首次默认并行工作区测试在并发负载下有五个依赖时间的 Dispatcher 断言失败；相同工作区串行测试通过。修复子进程已退出和被收养后代的边界后，格式、工作区 Clippy、串行测试与构建再次通过，见同目录的 `*-review-fix.log` 文件。此候选仍须独立实现审查、经审查的 MuJoCo 子进程提交和 IsOS 原始管道消费者，以及真实跨所有者首次使用重放，才能宣布 R06 已验证。

独立实现审查对 `e80c256e` 指出三个安全阻断。修订后的候选在 spawn 前记录 EOS 直接子进程身份；即使模拟器后代脱离原 session、leader 在首次停止扫描前退出，仍把新收养的子进程作为可能的模拟器后代。spawn 后立即锁定 leader 身份，并在就绪检查中避免提前回收，使启动回滚可向被锁定的进程组发信号。公开的受管理停止先登记 writer 并启动经认证、按 ID 键控的配套物理 worker，再获取 journal 互斥锁；内部紧急停止也先启动物理 worker，再执行有界的异步 journal 锁存。超过原始物理期限，或意图与部分结果尚未持久化时，操作不能 confirmed。确定性测试在运动 `Releasing` 阶段持有 journal 锁，验证公开与内部停止都先到达物理 lane，并覆盖提前收养的子进程及就绪前后代清理。

2026-11-02 执行 `/home/ashex/.cargo/bin/cargo fmt --all -- --check`、`/home/ashex/.cargo/bin/cargo clippy --workspace --all-targets -- -D warnings`、`/home/ashex/.cargo/bin/cargo test --workspace --no-fail-fast -- --test-threads=1`、`/home/ashex/.cargo/bin/cargo test -p aether-dispatcher --features test-hooks --test gateway_stop -- --test-threads=1`、`/home/ashex/.cargo/bin/cargo build --workspace`、`python3 tests/fixtures/simulator_stop/check.py` 和 `python3 scripts/check_doc_links.py --all` 均通过。完整输出保存在被忽略的 `logs/test/20261102-r06-review-blockers/`。此修订提交仍需独立复审与跨所有者重放。

## R04 集成更新

经审查的 R04 宿主服务与私有预检合入 `refactor(v2)/runtime-main` 的 `120b398c` 后，R06 分支基于该精确提交重新整理。共享关闭路径先执行物理停止，再有界关闭宿主 worker，并在终止 MuJoCo 前完成该关闭；只有关闭成功，受管理终端凭据才可能 confirmed。凭据回归明确验证：即使其余停止证明完整，只要宿主 worker 关闭未经确认，结果仍为 unknown。更新后的所有者检查通过：`cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、串行 `cargo test --workspace --no-fail-fast -- --test-threads=1`（301 项通过、一个既有忽略项）、带 `test-hooks` 的 Dispatcher `gateway_stop`（20 项通过）、`cargo build --workspace`、R04/R05/R06 固定用例，以及所有文档链接（318 个文件、零断链）。完整本地输出保存在忽略的 `logs/test/20261102-r06-r04-refresh/`。另一次基于重新整理前 EOS 提交 `ac331041` 的真实 IsOS→EOS→MuJoCo 重放确认 pick-place 与 navigation 停止，并将停止套接字丢失和子进程崩溃归为 unknown；见 IsOS `logs/test/20261102-r06-full-cross-owner/`。更新后的精确提交仍须独立审查和跨所有者重放，PR 才能合并。

## 已合并所有者与限定范围资格状态（2026-11-02）

EOS [PR #322](https://github.com/Island-Team/Island-eos/pull/322) 以 `1204c84849f7675e9f8393488f6f0ef60ee118d7` 合并。IsOS 精确提交重放使用经审查的 EOS `b3d6ab0147b8cf43d9012f860a5168bfc7a47`、IsOS `3567dadb1ce48d5cfef61c716fb906563336c8e8` 和 MuJoCo `c171208f0ef605fd4d5275d75567928713c50e96`，全部清理重建并使用锁定的 `island/mujoco-sim@0.1.1` 包。全新无界面 IsOS 主目录运行 pick-place 和 navigation：受治理的 `base.velocity` 与 `arm.movej` 完成，API Stop 在选中 EOS 原始管道发出双部分 stopped、epoch `1`、子进程组排空/回收、套接字删除及 EOS 正常退出的凭据。配套套接字丢失、MuJoCo SIGKILL、EOS SIGKILL、EOS SIGSTOP/强杀保持 unknown 或 degraded，授权已退休且无存活模拟器。精确提交 MuJoCo 停止相关 CTest 通过 7/7。完整命令及原始输出在主 IsOS 忽略的 `logs/test/20261102-r06-exact-heads/`。

后续已合并输入的负例重放使用 EOS `317e4520`、IsOS 可执行文件 `e34b7e7` 和 MuJoCo `c171208`，并核对二进制输入。bridge 子进程 stdout 上伪造的 confirmed 凭据未覆盖选中 EOS 管道。替代配套端点接收真实 stop 帧却不回应；EOS 约三秒后把两部分物理结果归为 unknown，IsOS 在 3.457 秒内返回 `unknown/eos_stop_unconfirmed`。这次已完成的 unknown Stop 后，管理器 SIGKILL 与重启保留 degraded/unknown，并以 `deployment_stop_unconfirmed` 阻止 Restart。真实引擎未服务替代停止套接字，因此该用例只证明超时分类，不证明物理保持。见主 EOS 忽略的 `logs/test/20261102-r06-final-negatives/SUMMARY.md` 及完整原始日志。

这些重放支持已合并的所有者路径和选定负例分类。它们尚未跨所有者实时证明物理部分停止、Stop 进行中管理器崩溃，或就绪后 GUI 渲染阻塞；畸形凭据和继承管道用例只有聚焦所有者或替身证据。部分 stopped 表示引擎提交底盘零驱动或机械臂保持及 epoch 栅栏，不表示残余速度在数学上为零。原始 R06 所有者条目与限定范围 M3 完成条件已由[M3 记录](../../../island/m3-qualification_cn.md)验收；更广泛的实时用例和 M5 仍未完成。

## Q04 后端后代归属修正

启动后新增的 EOS 直属子进程不能单凭时间归给后端：宿主 skill worker 与适配器宿主也可能晚于后端启动。停止只可向身份已证实的进程发送信号或回收：PID 与启动 ticks 匹配、属于被持有后端 PGID/session 的成员，或其存活父进程已证实的后代。子进程被 subreaper 收养后仍保留该身份。若新增 EOS 直属子进程不属于已证实集合，EOS 不触碰它，并以 `backend_group_unreaped`（unknown）结束。父进程先退出后才被观察到的脱离后代无法与无关 worker 安全区分，也保持 unknown。独立 session 的无关子进程回归测试须证明停止既不发信号也不回收它；已收养脱离后代测试须证明 unknown，原进程组成员和此前已观察的后代仍按期限处理 TERM/KILL。

归属不明的收养进程出现时，已证实的进程组及后代仍可排空。随后 EOS 不再向组发信号，安全回收被持有的后端 leader，但凭据保留 `backend_group_drained=false` 和 `backend_group_unreaped`，不报告 `backend_forced_kill`。回归测试核对准确原因、leader 回收及无关子进程仍由原调用者持有。运行时启动顺序先于后端基线创建适配器宿主；受管理宿主 skill 关闭会在后端终止前等待。未回收的宿主 worker 已令终端凭据为 unknown，归属不明不能把它升级为 confirmed。

## Q04 受管理启动停止证明修正

干净公开快照检查暴露了启动竞态：模拟器主套接字可先于配套停止 listener 绑定而回答严格健康探测，EOS 却已输出就绪。现在 IsOS 受管理运行要求每个声明的底盘／机械臂在两个套接字上以同一 generation 证明 `strict-stop-v1`，然后才报告就绪。包括尚未连接的启动套接字在内的暂时性传输错误只在既有绝对启动期限内重试；证明格式错误或 generation 不一致立即失败。独立只读 R05 启动保留可选停止证明，只有相机的图保持只读。

进程回归测试延迟绑定配套 listener，验证就绪发生在证明之后；始终未绑定的 listener 必须超时并产生 `eos_start_failed`，没有就绪输出，且套接字已清理。聚焦传输测试将 `NotConnected` 归为可重试、截断帧归为无效。第一次完整测试暴露了三个只提供主套接字的历史只读单元夹具；仅在受管理运行强制证明后保留了该兼容边界。失败及最终完整输出均保存在被忽略的 `logs/test/20261104-q04-stop-readiness/`。使用 `TMPDIR=/home/ashex/q04t/tmp` 和隔离的 `CARGO_TARGET_DIR=/home/ashex/q04t/eos-stop-readiness-target`，`cargo fmt --all -- --check`、工作区 Clippy `--all-targets -- -D warnings`、串行 `cargo test --workspace --no-fail-fast -- --test-threads=1`（378 通过、1 忽略）、锁定依赖的工作区二进制构建、模拟器停止夹具检查及全部文档链接均通过。公开快照仍须从审查后合并的源码重新选取并测试。
