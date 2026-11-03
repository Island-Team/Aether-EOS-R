# Q01 模型故障与显式重新启动

本文记录 Q01 中的新运行实例故障与恢复验证。[跨仓库待办](../../docs/island/implementation-todo.md)中的 Q01 仍未完成；完整关卡还需在一次流程中验证安装、选项、界面、无效版本和显式迁移。此处链接已验证的 MuJoCo 包，不能替代全新安装验证。

## 所有者契约

EGIA 拥有模型驱动任务及持久终态。模型以 HTTP 200 返回无效控制命令时，计入所选配置 `decision.max_invalid_replies` 的次数；失败任务不得提交提案或生成 EOS 移动日志。IsOS 拥有进程控制 Restart，先停止并回收旧部署，再分配新的运行 ID 和控制授权。EOS 拥有 Dispatcher 日志；新授权查询旧运行的操作必须不可见。图原始字节及活动修订保持不变，新任务使用新的操作 ID，不重放旧动作。

所有者线协议保持不变：参见 [EOS gRPC 能力契约](../eos/interfaces/grpc_cn.md)，以及所记录源码版本中的 IsOS `docs/isos/interfaces/control-grants.md`、`docs/isos/interfaces/dependent-stop.md`，EGIA `docs/egia/interfaces/pipeline.md`、`docs/egia/interfaces/safety-reset.md`。本文只记录跨所有者重放条件，不增加 API 字段。

故障模型桩仅对控制器请求返回 `INVALID_Q01_REPLY`，HTTP 状态为 200；哨兵通道保持正常回复。桩只记录有限的请求元数据与模式切换，不记录提示词、授权头或授权内容。只有故障任务确认为 `failed` 且无移动检查通过后，才恢复正常模式。

## 重放设计与证据

`tests/m4_affected_replay.py --mission --q01-restart` 创建独立 IsOS 主目录，选择固定版本的图，Validate/Apply 后启动 EOS/MuJoCo 和 EGIA，先记录一次受管移动。随后注入无效回复，记录每次解析失败与达到上限事件，确认 EOS 日志没有新增移动，执行受管 robot Stop，并通过 IsOS HTTP 接口显式 Restart。证据包括新旧状态、进程 ID、授权摘要、修订、EGIA 就绪、旧 ID 查询和旧授权拒绝。新任务必须产生一个不同 ID 的 EOS 终态记录。最后再次执行受管 robot Stop 与确认的 IsOS Stop。

Restart 后，重放等待新 EGIA 进程的北向 primitive 目录显示可用的 `base.velocity@robot-a/drive` 绑定。仅有 `egia_ready` 不足以证明规划器可见该动作；过早提交任务会被判为未知 primitive。目录帧连同运行 ID 和 EGIA PID 一起归档。路由投递是稍后的汇总事件，不作为就绪门槛。

EOS 日志记录一个准入的类型化命令及适配器终态确认。MuJoCo 严格套接字的去重状态仅保存在内存中，没有只读执行历史接口，因此本归档不能独立统计模拟器执行次数。已有 R05 严格套接字测试覆盖同 ID 去重。

`tests/verify_q01_restart.py <archive>` 仅从归档验证图原始字节、所选覆盖层、所有者事件、两次任务、控制器故障元数据、授权摘要、运行顺序、日志文件摘要和停止状态，不依赖临时主目录。修改归档中的故障次数、运行 ID、操作记录或摘要应被拒绝。归档仅保存授权摘要，不保存授权原文。

本切片不证明同一运行中替换 EGIA 子进程，也不证明新授权下使用旧 ID 结算，更不代表 Q01 全部完成。所选 SIGKILL/锁存负例及 EGIA 稳定运行下的进程恢复证据另见 [M4 重放](m4-affected-replay_cn.md)。

## 首次使用命令

以归档 `selected-inputs.json` 中的源码与二进制哈希为准；可将命令里的工作树路径替换为相同哈希的本地副本。完整命令及原始输出保存在忽略的 `logs/test/20261102-q01-restart/live-*.log`。完成后运行 `python3 tests/verify_q01_restart.py <archive>`。`q01-summary.json` 记录两个运行 ID 和不同的操作 ID，`q01-planner-fault-events.json` 记录有限次数的故障，`q01-restart-response.json` 与 `stop-response.json` 记录所有者结果。

## 选定重放与检查

本切片选定 `logs/test/20261102-q01-restart/live-8/`，环境为 Linux x86_64、Intel i5-12490F、Rust 1.98.1。源码版本为 EOS `1b850f56`、IsOS `2bdf6ea0`、EGIA `0c0c7670`、IsosUI `6afd5bdc`；MuJoCo 发行包摘要见 `selected-inputs.json`。`command-argv.json` 保存完整重放参数；`live-8.log` 以 `Q01_RESTART_SCOPED_PASS` 结束。新旧运行 ID 和移动操作 ID 不同，最后 IsOS Stop 为 `confirmed`。修正后的收尾程序直接归档完整 EGIA 私有日志；离线验证器检查故障摘录、三个原始 `1.1.0` 控制记录校验和、新任务前的规划器目录就绪、EOS 日志身份和子进程回收。

在 EOS 工作树执行 `python3 tests/verify_q01_restart.py logs/test/20261102-q01-restart/live-8` 及 `PYTHONPATH=tests python3 -m unittest test_m4_model_stub`。后者需要 `PYTHONPATH=tests`，因为测试将 `m4_model_stub` 作为顶层模块导入。完整输出保存在同一忽略目录下的 `verify-live-8.log`、`model-stub-final-pass.log`、`fmt-final.log`、`clippy-final.log`、`workspace-test-final.log`、`build-final.log` 和 `doc-links-final.log`。执行命令为 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace --all-targets -- --test-threads=1`、`cargo build --workspace`、`python3 scripts/check_doc_links.py --all`，均通过。改变原始控制记录的硬链接归档副本被验证器拒绝，证据见 `tamper-control-live-8.log`。这仅证明 Restart 切片；完整的全新主目录 Q01 流程仍未完成。
