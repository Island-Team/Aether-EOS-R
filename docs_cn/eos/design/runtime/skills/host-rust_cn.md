# R04 Rust 原生技能宿主

[English](../../../../../docs/eos/design/runtime/skills/host-rust.md) · [ABI 与选择契约](../../../interfaces/skills/native/host-abi_cn.md) · [SkillService](../../../interfaces/skills_cn.md) · [首次使用](../../../develop/host-skills_cn.md) · [用例](../../../../../tests/fixtures/r04_host_skills/README_cn.md)

**实现状态。** 默认 Rust runtime 已实现 R04 验证过的显式选择、受约束原生 worker、有界执行管理器、primitive Broker、七方法 SkillService 与就绪准入，并发布 `host-skill-abi-v2`。IsOS 已实现停止旧服务代之前的候选预检及可检查的 grant 退休。精确合并版本的实时替换重放覆盖旧运行停止确认与停止结果未知时的故障关闭；完整实时 90 秒超时及预检后可执行文件／库变更不属于该次重放。可选 C++ `src/eos/runtime/skills/` manager 与 worker 等待 Q02 末次调用者检查。`crates/aether-runtime/src/bin/aether-runtime/skills.rs` 中的历史图技能序列辅助代码没有部署的受管 `skill_execute`、`skill_result` 或 `skill_cancel` 路由；R04 宿主目录与 SkillService 不调用该代码。R03 提供运行期授权、活动修订与持久 Dispatcher；IsOS 提供精确软件包验证和已选择的清单。

## 启动屏障

1. IsOS 验证显式的 `settings.skill_libraries` 图声明、P01 收据和库文件字节，生成 `isos/skill-selection.json`，并在停止旧子进程前检查实际选中的 `aether-runtime` 是否支持 `host-skill-abi-v2`。
2. EOS 先验证 R03 已选择的 profile 与授权校验文件，再验证同一清单中唯一的 `isos/skill-selection.json` 条目。不完整交接必须失败。没有选择时，目录为空且不可执行。
3. EOS 对每项验证收据、release、完整 payload 树、精确库摘要和安全路径；以禁止跟随符号链接的方式打开库，并将该描述符中的已验证字节复制到仅所有者可访问的只读临时文件。它解析 ELF 依赖表，拒绝 RPATH/RUNPATH 或任何软件包本地、未列出的传递依赖；仅允许[所有者契约列出的可信系统 SONAME](../../../interfaces/skills/native/host-abi_cn.md)解析。父进程只解析数据，绝不加载软件包代码。
4. EOS 以私有 `--skill-worker` 模式重新执行选中的 `aether-runtime`。父进程先 `env_clear`，只传明确允许的最低环境，并将其他描述符设为 close-on-exec；子进程只接收一个 `SOCK_SEQPACKET` 通道和一份暂存库描述符，不得继承 R03 grant、IsOS `env_from` 秘密、runtime 或设备 socket。子进程先建立 `no_new_privs`、资源限制、Landlock 和 seccomp，再执行 `dlopen` 与 `eos_skill_library_query_v2(2)`；强制措施失败则不能加载。静态构造函数和查询代码可能在受约束 worker 中运行；描述符无效时绝不调用 `execute`。这里证明的是受约束且故障关闭的准入边界，并非通用沙箱完全隔离证明。
5. 父进程将所有库的有界描述符作为一次原子目录准入。全部通过后才绑定公开 SkillService 并报告 ready。失败候选的 worker 必须在本 EOS 子进程报告 ready 前回收。已实现的[私有候选预检](../../../interfaces/skills/native/host-abi_cn.md#私有候选目录预检)会在 IsOS 停止旧服务代前执行同一目录检查；这项跨所有者行为已经实现，其可检查退休边界有精确合并版本的实时证据。不得使用 C++ loader 的递归目录扫描。

私有 worker 使用版本化有界消息、`SO_PEERCRED` 验证启动的对端，并且没有公开监听器或继承的硬件 socket。协议、对端或尺寸不匹配将拒绝会话。C ABI 保持原样；内部 Rust worker 协议仅在同一 EOS 构建内有效，可以独立演进。worker 和父进程不共享软件包指针；worker 得不到 runtime、Dispatcher、设备、图编辑或 workflow 句柄。目录检查期间描述符没有 grant。


## 替换前候选检查

EOS 私有命令、共享静态校验器、IsOS 停止前调用及可检查的候选 grant 退休均已实现。精确合并版本的停止确认与未知停止替换重放见[首次使用指南](../../../develop/host-skills_cn.md#r04-精确合并版本可检查退休验收2026-11-02)。

IsOS `restart_processes_with_plan` 在保留旧 supervisor 时分配候选运行，完成 P01/P04/P05 与进程规格检查，并以候选 run ID、生成 profile、节点贡献、选择文件和 verifier 调用所选 EOS 的 `--skill-catalog-preflight`。旧运行停止确认前不发布候选运行。失败走可检查的候选 grant 退休；退休确认时保留旧运行，退休不确定时明确报告类型化失败。预检成功后，IsOS 复核输入，确认旧运行 R06 Stop，再发布并启动候选。精确调用、JSON 结果、边界及错误优先级以所有者契约为准。

预检在常规 runtime 构建前进入受限路径。它读取受管 verifier 与 profile 数据，复用启动时的已选目录准入和共享的纯数据就绪校验器。该校验器抽取当前 `Catalog::validate_broker_coverage` 与 `HostSkillManager::new` 的门槛：已声明 callback 覆盖、公开描述符投影及编码后的 3 MiB `ListSkillsResponse`，以及通过 `admitted_bindings(profile)` 与投影的 profile 级资格规则为每项已声明 primitive 找到静态 binding。实时终态停止围栏、adapter 与 health 检查仍在实际启动和调用时执行。此模式不能绑定 IPC/gRPC、构建 Runtime、Projection、adapter 或 Dispatcher、打开 journal、启动 MuJoCo、接收 SkillService 调用或调用 guest handler。父进程只解析有界复制出的 worker 元数据；构造函数／查询代码仍在受约束 worker 内。它在全部受约束检查子进程与暂存文件消失后退出。IsOS 对整个预检实行硬性 90 秒期限，可能拒绝接近资源上限但本身有效的软件包。超时或无法确认检查子进程回收均在停止旧运行前失败。R05 的延迟物理释放及 R06 的停止顺序仍需分别验收。抽取出的校验器单元测试须覆盖缺少 base/arm binding、声明 callback 不可用、公开编码恰好达到／超过 3 MiB，以及同一有效 profile 上预检与启动结果一致。

预检只检查某一时刻的输入，不预留启动许可。IsOS 比较结构化结果与已选 generation，并在替换前立即复核保存图字节、manifest 文件清单、所选可执行文件、候选 verifier 与全部 P01 输入。预检后的停止前检查失败也须明确退休候选 grant。新 EOS 启动独立重新打开并暂存文件，在 ready 前重复 ABI 与纯数据就绪准入；预检后的变更默认拒绝，并沿用现有候选恢复路径。相同运行中的自动子进程重启也针对同一已选 generation 重复启动准入。测试必须证明错误 ABI 或缺失 binding 的候选保留旧进程和 grant 可用，接近期限的超时清理不停止旧运行，且预检后的库替换在启动时被拒绝。

此前的纯设计切片以已独立评审的 EOS 服务提交 `e61f666e4b3f9ffb9dceda03abc63faa7af0d5ca` 为起点。2026-11-02 的 `git diff --check`、`python3 tests/fixtures/r04_host_skills/check_cases.py`（27 个宿主用例）、`python3 -m json.tool tests/fixtures/r04_host_skills/preflight_cases.json` 及变更文档相对链接检查均通过；完整输出见本地忽略的 `logs/test/20261102-r04-preflight-design/docs-check.log`。这些检查仅验证文档形状。当时独立设计评审、实现、IsOS/EOS 重放与 R04 验收仍未完成。

首次独立设计评审指出了零技能计数不匹配、在目录准入后才执行的就绪门槛、候选 grant 退休结果未检查、错误代码集合不封闭、90 秒期限被过度保证以及预检 fixture 未纳入校验。本次修订针对这些设计问题；`git diff --check`、现已检查八个预检用例的 fixture 脚本、JSON 解析、Python 语法编译及变更文档相对链接检查均通过，输出见 `logs/test/20261102-r04-preflight-design/review-fix-check.log`。当时设计就绪前仍需独立复审。

## 调用与 Broker

Rust SkillService 门面实现现有 `island.eos.v1` proto 中的 `ListSkills`、`GetSkill`、`StartExecution`、`GetExecution`、`WatchExecution`、`ListExecutions` 和 `TerminateExecution`。其他方法保留 proto 身份，直到经评审的完整实现前返回 `UNIMPLEMENTED`。List/Get 暴露排序且不可变的准入目录。Start 校验精确身份、object schema、request ID、可选超时、R03 EGIA grant 和活动修订，预留幂等 execution，有 worker 槽时才启动受约束子进程。声明 claim 的描述符在 Rust claim 调度器实现前拒绝目录准入；R04 不发布未执行的 claim 模板。非重入描述符按精确 `{name,version}` FIFO 串行，排队时仍受期限与取消检查，不能提前启动 worker。每次活动执行仅能得到精确声明的 primitive 与已准入绑定的有界 Broker grant。guest 只取得 ABI callback；host callback 经私有 IPC 与 Broker。未声明家族的可选 ABI callback 可为 `NULL`，按 ABI 规则报告不可用。

Broker 对 primitive 请求核对已准入描述符、精确 primitive/version、解析后的设备、execution、绑定代与截止时间。它按[所有者契约中的域分离哈希](../../../interfaces/skills/native/host-abi_cn.md)由 IsOS `run_id`、精确技能身份、公开 Start 的 `request_id` 和调用序号派生 67 byte 私有 command ID，再以服务端确认的 `skill-worker` 入口及 Start 授权角色进入 R03 Dispatcher。同一运行跨 EOS 子进程重启得到相同 ID，新运行得到不同 ID。Dispatcher 在物理释放时重新检查活动修订、健康、观测、限制、日志和安全状态。Broker token 仅限内部，不出现在公开 proto、worker 日志或软件包数据中。worker IPC 消息不能直接调用原始 adapter。R04 必须实现 primitive 路径。在资源、推理、source、SIGNAL 或 community callback 家族拥有完整 Rust 实现前，声明它们的描述符必须整体拒绝，不能以可执行技能出现在 `ListSkills`。未声明家族的可选 callback 可为 `NULL`；log/checkpoint 的可用性由 handler 按 ABI 检查。子进程 callback 在 0.x 不桥接。

执行记录仅在进程内且有界。保留期内相同的 `request_id` 返回原有 `execution_id` 和快照；冲突重用不启动 worker。进程重启后不得重放执行表来重新调用 guest。Dispatcher 的日志身份仍阻止同一派生 ID 下结果未确定的物理命令再次释放。Start 与 worker 返回体在公开前校验 schema、大小并脱敏。`WatchExecution` 跟随生命周期序号；客户端断开只取消 watcher。worker 退出、终态结果和 claim 释放各发生一次。

## 取消、故障和可观测性

单次执行终止先关闭其 Broker 准入并撤销 grant，取消活动 Dispatcher command ID，有限等待终态或明确的未知结果，再通知 worker，必要时强制终止并回收。执行器可能运动但没有终态结果时，EOS 映射为 `EXECUTION_STATUS_FAILED` 与 `worker.outcome_unknown`，不将其改成成功，也不自动重试。排队中的 execution 可以不启动 worker 而取消。整个 EOS 关闭有更强的安全顺序：先 `grpc_server.begin_shutdown` 并关闭宿主入口，`dispatcher.quiesce` 封闭新运动，随后 `stop_guard.stop` **立即**发出已准入执行器停止，不能等待技能 worker 的宽限期。再撤销 Broker grant、经 Dispatcher 取消剩余 command ID，并在模拟器仍运行时有界回收 worker。旧 `skills.shutdown` 在受管部署中没有活动工作，不得阻挡停止。只有 worker 已回收或明确记录停止故障，R06 才可由 `drop(backend)` 终止/回收 MuJoCo；物理停止失败不能报告为安全。OS 子进程尚未实际回收时，不得发出“停止完成”事件。现有阻塞 worker fixture 证明 `TerminateExecution` 与回收有界；含模拟器子进程的进程级停止顺序测试仍是 R06 集成门槛。 R04 取消仍可能与 Dispatcher 已进入 `Releasing` 的命令竞争；此时记录 `OUTCOME_UNKNOWN` 并保持运动抑制。若 adapter 运动在 stop 后延迟执行，终态物理围栏属于 R05/R06 边界，必须在该阶段证明后才能声称模拟器安全停止。

共用 JSONL 事件带部署 `run_id`、execution/correlation ID、安全的软件包 pin、阶段、reason code 和 worker 回收结果。事件不得包含软件包路径、原始描述符、参数、结果体、凭据、token 或库字节。IsOS 准入这些事件代码后，P07 日志轮转必须保留首次故障或 ready 事件。[R04 冻结用例](../../../../../tests/fixtures/r04_host_skills/cases.json)区分 worker 启动前数据拒绝、受约束查询拒绝、guest handler 禁止、Broker 拒绝、物理结果未确定和有界清理。旧 C ABI 前缀与可选尾字段用例需按现有可选实现比对精确 C 布局与查询符号。R04 关闭前完整输出放在 `logs/test/<run-id>/`。

已选择的 C guest fixture 使用带 build metadata 的 P01 pin 和真实宿主事件接收器，验证有界生命周期 JSONL。IsOS 现已在封闭的子进程事件策略中接收 `eos_skill_execution` 与 `eos_skill_stop_unconfirmed`。精确合并版本的重放通过测试程序触发的日志轮转保留了首次就绪与未知 Stop 故障；这不代表已测试自动大小阈值轮转。

## R04 已选 pin 事件修复证据（2026-11-02）

环境为 Linux 7.0.0-38-generic x86_64、rustc 1.98.1。从 EOS 主题 worktree 根目录运行下表命令，退出码均为 0。完整 stdout/stderr 保存在被 Git 忽略的 `logs/test/20261102-r04-skill-service/` 目录对应日志中。

| 命令 | 退出码 | 日志 |
| --- | ---: | --- |
| `cargo test -p aether-common skill_lifecycle_and_stop_failure_use_safe_jsonl_fields` | 0 | `pin-event-common.log` |
| `cargo test -p aether-runtime selected_skill_pin_matches_jsonl_bounds_and_p01_version_characters` | 0 | `pin-event-catalog.log` |
| `cargo test -p aether-runtime --test host_skill_catalog -- --nocapture` | 0 | `post-spawn-guest-final.log` |
| `cargo test -p aether-runtime lifecycle_event_failure_prevents_clean_host_stop` | 0 | `event-failure-latch.log` |
| `cargo fmt --all -- --check` | 0 | `fmt-final11.log` |
| `cargo clippy --workspace --all-targets -- -D warnings` | 0 | `clippy-final11.log` |
| `cargo test --workspace` | 0 | `workspace-test-final11.log` |
| `cargo build --workspace` | 0 | `workspace-build-final11.log` |

C guest 日志包含精确 `package_version:0.1.0+build.1` 的 `eos_skill_execution` 排队、运行和完成事件。最初合并多个 Cargo 测试过滤器的命令因 CLI 用法错误退出 1（`pin-event-focused.log`）；以上四个单独的聚焦命令取代了它。本地证据不关闭 IsOS 事件策略／游标重放或 R06 物理停止顺序门槛。

worker 启动、IPC 和协议故障在发布 `SkillExecution` 与 JSONL 前映射为固定的 `worker.failed` 或 `worker.protocol_invalid` 原因。内部 `HostError` 字符串和 guest 提供的故障文本不会复制进公开生命周期事件，使 IsOS 子进程事件策略仅需覆盖已评审的 `worker.*` 命名空间。

子进程已启动后的调用发送故障不能当成启动前错误。`Catalog::start_worker` 显式尝试终止／回收，并把观测到的回收状态返回 manager。`Some(false)` 继续占用 worker 槽和不可重入通道，使 `wait_stopped` 失败。已选 C fixture 以真实的启动后无效调用验证已回收路径；manager 单元 fixture 注入未回收结果，证明停止不能通过。

检查 worker 启动同样应用该清理规则。Inspect 发送或 `pidfd_open` 失败时有界尝试回收；未回收的检查子进程产生专门的 `eos_skill_stop_unconfirmed` 启动事件，阻止就绪及 backend 启动。负向辅助测试固定了传输错误与未回收错误的优先级，但没有模拟内核不可中断子进程。

启动后清理的后续检查也通过：`cargo test -p aether-runtime unconfirmed_inspection_reap_overrides_send_failure`（`inspect-unreaped.log`）、`cargo test -p aether-runtime unreaped_post_spawn_failure_keeps_worker_slot_and_fails_stop`（`post-spawn-unreaped-final.log`）及 `cargo test -p aether-runtime --test host_skill_catalog -- --nocapture`（`post-spawn-guest-final.log`），退出码均为 0。最后一项验证真实子进程在无效 Invoke 后回收；模拟未回收用例保留活动槽并使停止失败。较早的 `clippy-final10.log` 因 `manual_inspect` 退出 101；把目录故障钩子改为 `inspect_err` 后，`clippy-final11.log` 退出 0。上表 `final11` 日志是最终完整门槛。

跨所有者事件设计评审发现，未回收的 worker 可能保留成功 RPC 状态并发布带 `worker.reap_failed` 的 `completed`。现在 EOS 在发出生命周期事件前强制记为 `FAILED`、清除结果并记录失败转移；worker 槽仍占用，宿主停止仍按失败处理。回归用例从 guest 成功但 worker 未回收的状态开始。受影响的格式、严格 Clippy、定向测试、串行 workspace 测试及 workspace 构建命令均退出 0；完整输出和命令位于 `logs/test/20261102-r04-reap-terminal/`。在该修复切片时，IsOS 事件策略与游标重放仍待完成。
