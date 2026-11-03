# R03 准入首次使用与验证

[English](../../../docs/eos/develop/admission-revision.md) · [所有者契约](../interfaces/grpc_cn.md#r03-执行器受治理准入目标契约) · [设计](../design/runtime/admission-revision_cn.md) · [用例](../../../tests/fixtures/r03_admission_revision/README_cn.md)

**R03 经所有者合并和合并后重放验收。** EOS 验证 schema-2 代际及按运行固定的授权；IsOS 签发并交付授权。下文为可复现的首次使用重放。R05/R06 单独验收 MuJoCo 物理去重、隔离及停止/回收顺序；后续[限定范围 M3 验收](../../island/m3-qualification_cn.md)已关闭完成条件。

1. 准备全新 IsOS 主目录并安装精确已验证的模拟器包。选择场景及通用相机/底盘/机械臂节点，再 Validate 和 Apply 完整图，记录 IsOS 保存整图修订。schema-2 生成快照应位于 `data/generated/<deployment>/v2/<revision>/<generation>/`；验证 `manifest.json` 与列出的 `eos/graph.json` 字节。旧 `1.0.0` 图可检查并显式迁移，但旧布局不能授权运动。
2. 通过 IsOS 启动受管理 EOS，提供所选 `/v2/<revision>/<generation>/eos/graph.json` profile、`EOS_CONTROL_GRANTS_FILE` 验证文件路径及匹配的 EOS `--run-id`。确认完整发现后 health 才是 `SERVING`，分别读取 `eos-active-revision` 与 `eos-projection-sha256` 响应元数据。前者必须等于 IsOS 活动保存字节修订；后者标识 EOS 投影字节，可以与前者不同。验证文件缺失、变化或不匹配时须在就绪前启动失败。EGIA/bridge 客户端授权文件缺失或变化，本身不改变 EOS listener 就绪状态；该客户端无法用缺失或被改动的授权通过认证。在 IsOS 运行轮换前，以前复制的旧秘密仍可能匹配验证文件。独立非 v2 profile 没有验证文件时，即使带可选 `--run-id` 仍只读。
3. 受监管 EGIA 或 bridge 进程只能从私有启动环境取得自己的授权路径。发送一个有界 primitive，带精确已发现 binding、角色授权、保存图活动修订、类型化参数、命令 ID 与截止时间。用同一授权和 ID 调用 `GetOperation`，比较终态 acknowledgement 的命令 ID、primitive/版本。Submit 超时或丢失响应时先查询同一 ID，再考虑新命令。不能从传输成功、图连线或 pending 结果推断物理成功。
4. 分别注入过期修订、错误角色、缺少或改动的客户端授权、错误 binding、过期健康/观测、数值超限与期限过期。每例 stub adapter 调用数应为零。占满运动通道后，确认已认证 `safety.estop` 在没有当前修订时仍走保留通道。RobotStop inhibit 期间，只有操作员授权加显式恢复标记及全新 ID 可尝试旧/当前目标并集，且不能清除紧急 latch。
5. 经 IsOS 日志先找到本次运行第一条失败或就绪事件，再按 `correlation_id` 与命令 ID 过滤。日志仅可含脱敏角色、修订及结果码，不能含授权字节或运动参数。注入 journal 故障及 `Releasing` 后崩溃；重启后 `GetOperation` 保留 `OUTCOME_UNKNOWN` 且 adapter 不自动重试。跨角色 Get/Watch/Cancel 不泄露结果，也不取消命令。
6. 经所有者路径停止并检查终态或未知结果。R03 以 stub 证明 Dispatcher 准入与 journal；R05/R06 独立关卡通过后才能记录物理停止与模拟器终止证据。保存草稿变化而未 Restart 不改变 EOS 准入修订。Restart 后新命令先重新发现。通过 IsOS 修订检查迁移路径显式迁移旧图。

评审前运行 `python3 tests/fixtures/r03_admission_revision/check_cases.py`、`python3 scripts/check_doc_links.py --all`、用例映射中的假时钟与真实 listener 聚焦测试、`cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace` 和 `cargo build --workspace`。完整输出保存于 `logs/test/<run-id>/`，在本页记录精确命令、环境、用例到测试映射及独立评审。固定负载报告吞吐、中位数与 p99 前不作性能主张。

2026-11-02 仅设计验证：`python3 tests/fixtures/r03_admission_revision/check_cases.py` 校验了 25 个计划用例；`python3 -m py_compile tests/fixtures/r03_admission_revision/check_cases.py` 通过；`python3 scripts/check_doc_links.py --all` 检查 310 个 Markdown 文件，零损坏相对链接；`git diff --check` 通过。完整输出位于被忽略的 `logs/test/20261102-r03-design/`。这些命令只校验文档与冻结用例结构，不验收 R03 实现或 IsOS 授权所有者切片。

## EOS 主题证据与 R03 验收

2026-11-02，EOS `refactor(v2)/admission-revision` 主题分支在 Linux x86-64、Rust 工作区版本 `0.2.0` 下通过本地检查。在 EOS 工作树根目录执行下列命令；完整命令输出保存在被 Git 忽略的 `logs/test/20261102-r03-eos-integration/`（每条命令单独日志）。

| 命令 | 证据与结果 |
| --- | --- |
| `cargo fmt --all -- --check` | `fmt-check.log`；通过，无差异。 |
| `cargo clippy --workspace --all-targets -- -D warnings` | `clippy-workspace.log`；通过。 |
| `cargo test --workspace` | `test-workspace.log`；50 项测试结果，251 通过、零失败、一个刻意忽略的实时 MuJoCo 测试。 |
| `cargo build --workspace` | `build-workspace.log`；通过。 |
| `cargo test -p aether-runtime --test process -- --nocapture` | `logs/test/20261102-r03-process-boundary/process.log`；15 通过、零失败。 |
| `cargo test -p aether-http-bridge --test bridge` | `bridge-tests-v3.log`；5 通过、零失败。 |
| `cargo test -p aether-dispatcher` | `logs/test/20261102-r03-core-initial/dispatcher-test.log`；39 项单元、18 项集成、15 项 gateway-stop 测试通过，零失败；该目录 `case-map.md` 给出聚焦用例映射。 |

EOS 进程测试覆盖独立只读发现、受管理输入缺失或变化时就绪前拒绝、所选 schema-2 profile 和验证文件校验、活动修订与投影摘要发现、角色和修订拒绝、stub 上受治理的 EGIA 运动，以及原始私有路径拒绝。bridge 测试覆盖私有操作员授权转发及拒绝浏览器自行提交授权。现有 P06 事件流对受管理启动失败在就绪前报告 `start_failed`，且启动完成后才发送 `ready`；R03 不新增命令事件 schema。持久命令状态及恢复仍由 Dispatcher journal 和 gRPC Operation 投影负责，Get、Watch、Cancel 按角色与运行范围校验。授权字节不得进入日志或事件载荷。

此表仅记录使用夹具授权的 EOS 本地证据。下文的跨仓库重放及合并后主检出目录重放检查真实 IsOS 授权交付。R05/R06 仍负责 MuJoCo 物理隔离和停止/回收证明。

### EOS 最终评审代码提交

2026-11-02，独立实现评审检查的 EOS 代码提交为 `607eb0a93824c9488d17d38c9785c44f87ca4934`。精确代码的最终运行保存在被 Git 忽略的 `logs/test/20261102-r03-eos-final/`。在此 EOS 工作树根目录运行：

| 命令 | 完整输出 | 结果 |
| --- | --- | --- |
| `cargo fmt --all -- --check` | `fmt.log` | 通过。 |
| `cargo clippy --workspace --all-targets -- -D warnings` | `clippy.log` | 通过。 |
| `cargo test --workspace` | `test.log` | 50 项测试结果，252 通过、零失败、一个刻意忽略的实时 MuJoCo 测试。 |
| `cargo build --workspace` | `build.log` | 通过。 |
| `cargo test -p aether-runtime --test process` | `process.log` | 16 通过、零失败。 |
| `cargo test -p aether-http-bridge --test bridge` | `bridge.log` | 5 通过、零失败。 |

最终进程用例验证：受管理 Get 对其他角色的操作 ID 与不存在的 ID 返回相同的 `NOT_FOUND` 响应。代码评审确认，共用的受管理查询映射也覆盖 Get、Watch、Cancel 上的错误运行与已封存旧 ID；这些额外组合没有单独的进程断言。较早的历史非受管理进程内查询，对缺少准入身份的旧记录仍报告 `DATA_LOSS`，保留恢复诊断，但不会使该记录在受管理运行中可写。浏览器 bridge 转发 `primitive_typed` 时读取并再次校验私有操作员授权；仍拒绝浏览器提交授权字节。EOS 所有者切片已完成本地评审及精确代码关卡。下文的跨仓库重放已在本地通过；其后的合并后主检出目录重放验收了 R03。

### 本地跨仓库重放

2026-11-02，Linux x86-64 上的重放针对 IsOS 主题提交 `5c1cb27878d5594e9eda2f07765395e2830da045`、EOS 代码提交 `607eb0a93824c9488d17d38c9785c44f87ca4934` 及 EOS 文档提交 `8df7e01e29d1ff609846cd7b56145a05ee868541` 通过。脚本与完整输出保存在 IsOS 主检出目录 `/home/ashex/projects/isos/logs/test/20261102-r03-cross-repo/`；`r03-cross-replay.executed.py` 记录精确候选工作树运行；`r03-cross-replay.py` 与 `probe/src/main.rs` 保留主检出目录的重放源，`mock-base-grpc-pairwise.log` 记录精确候选运行，`generalized-candidate-pairwise.log` 记录同一候选二进制文件上的通用重放。

在已合并所有者提交并重新构建二进制文件的 IsOS 主检出目录执行：

```sh
cargo build --manifest-path logs/test/20261102-r03-cross-repo/probe/Cargo.toml --offline
ISOS_BIN=$PWD/target/debug/isos \
ISOS_AETHER_BIN_DIR=/home/ashex/projects/Island-eos/target/debug \
python3 logs/test/20261102-r03-cross-repo/r03-cross-replay.py
```

保留的探针 `Cargo.toml` 指向合并后的 EOS 主检出目录。独立的 `r03-cross-replay.executed.py` 保留通过证据时使用的精确候选工作树路径。重放自行创建临时 `ISOS_HOME`，不输出授权字节。

重放创建全新的 `ISOS_HOME`，使用精确选中的 EOS 候选二进制文件，并将 EOS `valid_bindings` mock 底盘用于 IsOS 三进程起始图。测试检查所选 V2 代际与活动保存修订、私有验证文件权限和按接收方隔离的环境变量，再用真实 gRPC 发现修订与投影摘要、提交已准入底盘速度，并经 Get 取回匹配的 operation ID。操作员授权不能通过 gRPC 提交运动，过期修订被拒，其他角色查询与不存在的 ID 得到相同的不透明结果。显式 Restart 保留保存修订并轮换运行 ID；更新后的脚本逐一断言 EGIA 授权和操作员授权均发生变化。新 listener 拒绝已退役的 EGIA 授权。Stop 在子进程停止后退役活动授权包。`mock-base-grpc-pairwise.log` 与 `generalized-candidate-pairwise.log` 中逐项授权及边界断言全部通过。此次候选重放以 mock 底盘覆盖受管理 EOS/IsOS 边界。下文合并后主检出目录重放验收 R03；R05/R06 单独验收 MuJoCo 物理隔离和停止/回收顺序。

### 合并后 R03 验收

EOS [PR #315](https://github.com/Island-Team/Island-eos/pull/315) 合并到 `refactor(v2)/runtime-main`，提交为 `dbd2e2c39a54236786577ace424c30e418ff3926`；IsOS [PR #52](https://github.com/Island-Team/isos/pull/52) 合并到 `master`，提交为 `df3c1f5692c9348c1cfb5b2ecb8b1abfa9fa82f2`。重新构建两个主检出目录后，在 `/home/ashex/projects/isos` 执行探针构建与通用重放：先执行 `cargo build --manifest-path logs/test/20261102-r03-cross-repo/probe/Cargo.toml --offline`，再执行 `ISOS_BIN=$PWD/target/debug/isos ISOS_AETHER_BIN_DIR=/home/ashex/projects/Island-eos/target/debug python3 logs/test/20261102-r03-cross-repo/r03-cross-replay.py`。探针构建与重放的完整输出分别位于 IsOS 主检出目录的 `logs/test/20261102-r03-cross-repo/post-merge-probe-build.log` 和 `logs/test/20261102-r03-cross-repo/post-merge-primary.log`。两个主检出目录的所有者构建输出分别为 EOS `logs/test/20261102-r03-eos-final/post-merge-build.log` 与 IsOS `logs/test/20261102-r03-control-grants-impl/post-merge-build.log`。

全新主目录重放通过所选 V2 代际和按接收方隔离的验证文件交付、真实 gRPC 发现与已准入底盘速度/Get、错误角色及过期修订拒绝、不透明操作查询、EGIA/操作员授权分别轮换、Restart 后旧授权拒绝，以及 Stop 后授权包退役。此证据在已合并提交上验收 R03 EOS/IsOS 所有者边界。mock 底盘不能证明 MuJoCo 物理代次隔离或停止/回收顺序；R05/R06 的物理证据和后续[限定范围 M3 验收](../../island/m3-qualification_cn.md)已在合并输入上完成。
