# R04 原生技能宿主首次使用与验收

[English](../../../docs/eos/develop/host-skills.md) · [ABI/选择契约](../interfaces/skills/native/host-abi_cn.md) · [SkillService](../interfaces/skills_cn.md) · [Rust 设计](../design/runtime/skills/host-rust_cn.md) · [用例](../../../tests/fixtures/r04_host_skills/README_cn.md)

**当前首次使用指南。** 默认 Rust EOS runtime 已实现经过验证的原生库选择、受约束 worker、primitive Broker、七方法 SkillService 与就绪准入；IsOS 已实现候选预检与可检查的退休。所选程序发布 `host-skill-abi-v2`。IsOS 负责软件包验证、显式选择、Validate/Apply 和部署日志。EOS 负责 ABI 准入、逐次执行 worker、Broker/Dispatcher 调用、SkillService 与实时健康。以下流程可复现；精确合并版本的可检查退休结果与剩余实时边界见本文末尾。

1. **准备干净主目录和软件包。** 构建默认 Rust EOS 程序和选定的 IsOS 程序。制作最小 ABI v2 `.skill.so`，包含 `eos_skill_library_query_v2`、object 参数/结果 schema、一个声明的 primitive 和可计数的 `execute` handler。最小 C fixture 只依赖可信系统 libc；C++17 fixture 可依赖文档列出的系统 `libstdc++`/`libgcc_s`。不得嵌入 RPATH/RUNPATH 或软件包本地传递依赖。将它发布为 P01 有效的精确版本，再通过 IsOS 安装或链接。记录精确收据、release、payload 和库文件 SHA-256。仅安装软件包不能使 `ListSkills` 出现可执行技能。
2. **选择并 Apply。** 在 EOS runtime 的 `isos.process.component@v0.1` 节点，将 `settings.skill_libraries` 设置为一条显式 `{package:{id,version},library}`。Validate 再 Apply。确认已选择 schema-2 清单同时列出 `eos/graph.json` 与 `isos/skill-selection.json`，且保存的 `active_revision` 匹配。确认所选 `aether-runtime --capabilities` 发布 `host-skill-abi-v2`；缺失 feature 或 P01 文件被修改必须在停止旧子进程前失败。不要增加技能序列节点或命令边。
3. **预检、启动并确认就绪。** 旧运行仍在服务时，IsOS 检查候选保存修订、已选 generation、精确 P01 字节、所选 runtime feature 与进程规格。非空技能选择先准备候选 R03 grant，再于停止旧子进程前调用[私有 EOS 目录预检](../interfaces/skills/native/host-abi_cn.md#私有候选目录预检)。确认一条结构化 `ok:true` 结果，修订、generation、run ID、选择文件摘要及有序验证库均匹配，并确认检查子进程已回收。错误 ABI、缺少部署 binding 或公开目录超过 3 MiB 时必须返回 `ok:false`，通过可检查的路径退休候选 grant，且旧 PID、run ID、活动修订和 grant 仍可用。随后 IsOS 复核输入并启动替换进程。实际 EOS 启动重新完成软件包、ABI 与共享纯数据就绪准入，暂存新的精确字节，全部描述符和七个 R04 方法就绪后才报告 SkillService `SERVING`。若启动失败，查看 manager-session 首次故障，确认没有 ready 事件或遗留 worker PID。下文的精确合并版本确认与未知 Stop 替换重放已验收此可检查退休边界。
4. **发现并执行。** 使用现有 typed gRPC `ListSkills`/`GetSkill` 身份查看精确描述符。从 Discover 元数据读取 `eos-active-revision`。调用 `StartExecution` 时使用 `authorization: Bearer <EGIA grant>`、`x-eos-active-revision: <保存的修订>`、精确 skill reference、object 参数及稳定的 1–96 byte `request_id`。保存返回的 `execution_id`；它与 request ID 和 IsOS 部署 `run_id` 均不同。从 `after_sequence:0` 跟随 `WatchExecution`，watch 断开后用同一运行期 grant 调用 `GetExecution`。确认声明的 primitive 仅产生一个 R03 Dispatcher 日志身份和一次模拟 adapter 调用；未声明 primitive 均为零。用 `ListExecutions` 查看保留快照。
5. **关联日志并停止。** 在按 execution/correlation ID 筛选前，通过 IsOS cursor 找到部署运行的首次 ready 或故障事件。确认 SkillExecution 终态 reason 与安全 JSONL 字段一致，且没有 token、路径、原始参数、结果或库字节。对运行中的 fixture 使用同一 EGIA grant 和活动修订调用 `TerminateExecution`。确认撤销 grant、Dispatcher 取消和终态或未知结果、限时回收 worker，然后执行 IsOS Stop。R06 另行证明模拟器终止/回收前的物理停止；R04 必须证明 worker 在该顺序前消失。
6. **负例与重启。** 执行每个 [R04 用例](../../../tests/fixtures/r04_host_skills/cases.json)及[预检用例](../../../tests/fixtures/r04_host_skills/preflight_cases.json)。在 IsOS 验证后注入错误 ABI、缺少 binding 与 3 MiB 列表溢出；每项都须保留旧运行。检查拒绝、畸形输出、超时及清理未确认对应的四个固定 IsOS 预检诊断。接近 90 秒的超时及候选 grant 退休失败必须保留旧进程和 run scope，并明确显示清理故障。在 `ok:true` 后替换库或所选程序字节，证明实际启动重新拒绝而不复用预检结果。相同 IsOS 运行中重启 EOS，检查重新准入和 Dispatcher 不重放；新运行检查 grant 轮换与旧运行不可见性。记录实际通过／失败和子进程 PID。静态 fixture 不是验收证据。

用 `python3 tests/fixtures/r04_host_skills/check_cases.py` 检查 fixture 形状，用 `git diff --check` 检查文档变更。R04 验收采用 Rust 单元与进程测试、C ABI 兼容 fixture、IsOS/EOS 跨仓库重放，并执行 `cargo fmt --all -- --check`、严格 workspace Clippy、workspace tests/build 及 IsOS Cargo 检查。完整命令与输出保存在 `logs/test/<run-id>/`；在评审与合并前，将实际结果及固定提交摘要写回本文。

### 宿主基础证据（2026-11-02；前置切片）

本地 `logs/test/20261102-r04-host-foundation/` 记录了 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace`、`cargo test --workspace -- --test-threads=1`、`cargo build --workspace` 和 `python3 tests/fixtures/r04_host_skills/check_cases.py`。最终运行均通过。进程测试编译旧前缀 ABI v2 C 库，选择精确的 `.skill.so` 字节并在默认 Rust 私有 worker 中执行；它检查环境、文件与网络约束，并拒绝库变化、错误后缀、无关但可由其他用户写入的 payload 文件、未支持的声明和 RPATH。首次默认 workspace 运行出现四个 dispatcher 并发时序故障，日志已保留；串行 dispatcher 重跑 39/39 通过，最终默认 workspace 重跑也通过。较早的串行 workspace 运行暴露本切片测试 fixture 缺失 @v0.1 port 元数据；修复 fixture 后最终串行运行通过。在该基础切片时，IsOS/EOS 跨所有者重放、SkillService/Broker 测试、实现评审与发布验收仍待完成。

### 独立评审修复（2026-11-02）

独立的 `logs/test/20261102-r04-host-review-fixes/` 证据记录评审修复后的格式、严格 Clippy、workspace 测试/构建、fixture 与 diff 检查。图测试现在解析真实的 `skill_libraries` 值，并拒绝缺失字段、额外字段、错误类型及 17 项输入。C 进程 fixture 拒绝无效 ABI 名称、SemVer、JSON Schema 和 guest 伪造的 catalog 消息；还检查 `.skill.so` 路径长度、io_uring/x32 禁止、重复 completion 以及独立的 pidfd 取消。阻塞对端单元测试检查五秒有界发送路径。EOS 现在检查精确 worker 架构，在父进程复核每个复制出的描述符与已编译 schema，遵循 IsOS 的 `glibc_min` asset 选择，并只将安全 package pin 附到内部目录描述符。在该评审切片时，跨所有者重放与 Service/Broker 验收仍待完成。

## R04 服务切片本地证据

本地 `logs/test/20261102-r04-skill-service/` 保存了 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test -p aether-runtime --test host_skill_catalog --test grpc_skills`、`cargo test -p aether-dispatcher skill_`、`cargo test --workspace` 和 `cargo build --workspace` 的完整输出。最终格式、严格 Clippy、定向集成测试、workspace 测试和构建均通过。保留了两次较早的 workspace 失败：第一次测试错误地把 JSONL 信封自身的 `node_path` 键当作敏感负载；第二次沿用 R04 前空 SkillService 必须不可用的断言。修正后最终运行通过。

已选择的原生 C fixture 验证七方法门面、grant/修订拒绝、UTF-8 请求身份、幂等、watch 生命周期，以及经受管 Dispatcher 的真实 `base.velocity` callback。handler 解析 `primitive.ack`，检查 `accepted`、终态和 `sk-` 命令 ID；父进程独立检查持久日志与单次 adapter 调用。首次原语被拒绝后，即使 guest 报告成功，后续有效调用也被锁闭。阻塞的非重入 handler 证明 FIFO 排队以及不启动第二 worker 的排队取消。Dispatcher 测试覆盖持久释放前撤销，以及释放后 adapter 阻塞时在取消预算内得到 `OUTCOME_UNKNOWN`。JSONL 测试覆盖安全生命周期字段与明确的 worker 停止失败字段。在该服务切片时，IsOS 轮转重放、Dispatcher 阻塞时的进程停止顺序以及跨所有者候选预检仍是开放门槛；后续合并版本的限定范围证据见下文。

同一目录中的最终完整门槛是 `fmt-final11.log`、`clippy-final11.log`、`workspace-test-final11.log` 和 `workspace-build-final11.log`，退出码均为零。较早的 `final3` 与 `final6` 日志记录增量检查。已选择的 C fixture 进一步在持久释放前阻塞观测（无 adapter 运动，终态为明确 `CANCELLED`）、在释放后阻塞 adapter（Terminate 有界，公开状态为 `FAILED/worker.outcome_unknown`），并拒绝超过 1 MiB 的结果体。每个运行中的 execution 现在只有一个受跟踪的取消任务；worker 调度把活动取消任务计入容量，关闭时同时等待取消任务和 worker 回收。终态或被淘汰的 execution 会取消期限计时任务。在该服务切片时，含模拟器的进程级停止顺序与 IsOS 日志轮转仍属独立门槛。

基础 PR #317 以 `647bdd09dcd7ccfd02b1f6534833bf8b3c651ab3` 压缩合并后，服务分支已基于该精确集成分支版本刷新。`logs/test/20261102-r04-service-refresh/` 保存完整输出和命令。`cargo fmt --all -- --check`、严格 workspace Clippy、已选 C guest 与 gRPC 集成测试、Dispatcher 技能测试、`cargo test --workspace`（284 项通过，一项忽略）、`cargo build --workspace`、R04/R05 fixture 检查、变更文档链接检查及 `git diff --check` 均通过。刷新保留了已合并基础中的 R05 严格 MuJoCo 变更，以及服务切片的有限 worker 回收和 Dispatcher 行为。这是刷新候选的证据；独立增量评审与最终跨所有者验收仍是合并门槛。

## R04 EOS 候选预检切片证据

本地忽略目录 `logs/test/20261102-r04-candidate-preflight-eos/` 保存了 `/home/ashex/.cargo/bin/cargo fmt --all -- --check`（`fmt-final.log`）、`/home/ashex/.cargo/bin/cargo clippy --workspace --all-targets -- -D warnings`（`clippy-final.log`）、`/home/ashex/.cargo/bin/cargo test -p aether-runtime --test host_skill_catalog --test grpc_skills`（`focused-final.log`）、`/home/ashex/.cargo/bin/cargo test --workspace`（`workspace-test-final.log`）、`/home/ashex/.cargo/bin/cargo build --workspace`（`workspace-build-final.log`）、fixture 形状（`fixtures-final.log`）和空白检查（`diff-check-final.log`）的完整输出。最终命令均以零退出。较早的 `fmt.log` 在执行必要的格式化前失败，已保留。

私有命令的 C fixture 确认成功时仅输出一行 JSON，包含已选修订、generation、run、选择文件摘要及有序验证库元数据。它拒绝 socket 参数、过期的候选 run、选择文件摘要变化、未绑定的已声明 primitive，以及先前预检成功后发生变化的库；新的启动准入同样拒绝变化文件。纯数据就绪测试覆盖缺少 base 与 arm binding，以及编码目录恰好达到和超过 3 MiB 的边界。阻塞的 ABI 查询在检查期限内终止并回收子进程，随后返回 `worker/worker_timeout`。此命令不构建实时 Runtime 或 listener。独立的干净主目录 `logs/test/20261102-r04-oversize-cross-owner/` 重放证明旧的类型化 SkillService 在候选拒绝和日志轮转期间继续工作；`logs/test/20261102-r04-toctou-final-heads/` 覆盖两种探测后字节变更窗口和同一运行中的自动重启。这些本地证据来自已评审但尚未合并的版本；当时最终顺序合并版本的重放及全部固定 PF01–PF08 案例的闭环评审仍待完成。

服务 PR #318 以 `176ef1325b9544b22c03d45c7c9f7c490fa9f02d` 压缩合并后，候选预检分支已变基到该精确集成版本。`logs/test/20261102-r04-preflight-refresh/` 的 `results.json` 保存命令与退出码，其他日志保存完整输出。`cargo fmt --all -- --check`、严格 workspace Clippy、定向 C guest 与 gRPC 测试、Dispatcher 技能测试、`cargo test --workspace`、`cargo build --workspace`、R04/R05 fixture 检查、变更文档链接检查及 `git diff --check` 均通过。英文和中文指南的两处冲突保留了已合并服务与候选预检两组证据；源代码没有冲突。当时独立增量评审及最终合并版本的跨所有者验收仍是该 PR 合并前的门槛。

## R04 精确合并版本可检查退休验收（2026-11-02）

本地封存记录为 `logs/test/20261102-r04-exact-merged-tip/qualification.md`；`commands.txt`、`verification.json`、`binary-inputs.sha256`、逐项输出及 `evidence.sha256` 保存命令、精确输入和结果。全新分离检出构建使用 EOS `317e45207cc26c20b3f18ba2b0afe06b1b71fb4f`、IsOS `e34b7e7358d405a76822b5855f9adc6825cf218c` 与 MuJoCo `c171208f0ef605fd4d5275d75567928713c50e96`。IsOS 仅文档的 [PR #59](https://github.com/Island-Team/isos/pull/59) 随后合并，没有 runtime 源码变动；此记录验收的是 `e34b7e7` 可执行文件，不声称重新构建 #59 的程序。

旧 EOS 与真实 MuJoCo 子进程就绪后，旧运行 Stop 确认案例在 100.153 秒后返回 HTTP 200，仅发布一个候选并达到就绪；旧模拟器已回收。删除旧运行 `.stop` socket 后，Stop 在 105.521 秒后返回类型化 HTTP 422 `deployment_stop_unconfirmed`。旧运行及活动修订保持降级状态，没有发布或启动候选，候选授权已退休，旧模拟器已回收。停止事件共用关联 ID `p12`。同一 manager 四次以及 manager 重启后两次 Start/Restart 重试均返回类型化 HTTP 422；测试中候选图字节依次为畸形、缺失及恢复。测试程序触发的保留日志轮转保留首次 `eos_ready` 锚点与 `deployment_stop_unconfirmed` 故障。内存中的精确 token 检查扫描了 62 个证据／JSONL 文件，没有发现旧或候选授权 token 字节。

另一项基于精确源版本的测试在预检后、旧运行停止期间修改保存图，返回 HTTP 422 `saved graph revision changed`。它发出 `deployment_starting`，但候选从未 `deployment_ready`；因为保存图被刻意修改，恢复保持降级。这证明启动复核拒绝该修改，不能声称候选从未发布。源码规定 90 秒预检期限；`probe_timeout_reaps_group` 用缩短的测试期限及子进程清理 hook 通过。该精确版本重放未运行完整实时 90 秒超时，也未注入预检后可执行文件或库变更。先前受控变更运行与定向测试提供独立证据。这些限制保留到 M3/M5 总验收；R04 所有者实现与可检查退休边界已完成。
