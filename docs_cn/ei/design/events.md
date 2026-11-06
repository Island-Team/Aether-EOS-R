# P06 EGIA 事件生产者设计

[English](../../../docs/ei/design/events.md) · [归属接口](../interfaces/events.md) · [夹具](../../../ei/tests/fixtures/jsonl_events/README_cn.md)

**P06 生产者已实现。** [归属接口](../interfaces/events.md)定义线协议。本页将其映射到当前 EGIA 入口并保留私有 replay。P06 只改变日志，不实例化 A01 阶段节点或 R02 Gateway 命令。

## 入口和就绪

`src/egia/main.py::main` 解析所选配置并调用 `configure_logging`。目前 `src/egia/lib/logger.py` 随机生成 `run_id`，可向 stderr 写人类文本，写轮转归属 replay，并可发送旧 Island 会话 datagram。目标实现在托管事件 sink 配置前解析 `ISOS_RUN_ID`。`--managed` 要求有效 `r...` ID，缺失或错误时不发送 JSONL。独立模式拒绝意外的管理器绑定，一次生成 `l<uuidhex>`。同一 ID 注入归属 replay 和新的共享投影。安全 sink 建立前的 bootstrap 异常留在私有 `main.log`；IsOS 观测所选子进程退出并发出自身失败事件。

事件投影是在现有脱敏器之后的独立 handler。它将明确的生命周期、任务、安全和 EOS 结果调用点映射到封闭角色策略，不透传或重命名任意现有 `event` 字符串。托管 stderr 只含严格 JSONL；该路径禁用人类 stderr 和旧会话 datagram。归属 replay 仍在 `var/lib/egia/egia.jsonl` 独立轮转；人类日志仍在 `var/log/egia`。IsOS socket 不能替代所选管道。共享输出失败时在私有处记录有界计数，继续停止和回收。

配置、契约注册表、托管运行绑定和事件 sink 成功后发 `egia_starting`。只有完整 EOS 发现代际发布、必需模型绑定可用、安全门／监控初始化且 northbound listener 已绑定后，才一次发 `egia_ready`。该 ready 不代表任务成功。当前启动可选择空模式或 sandbox 后备；托管 EGIA 在要求已验证 EOS 路由时不得为这些路径发 ready。sink 建立后的启动故障用封闭原因发一次 `egia_start_failed`；建立前由 IsOS 监督报告。关闭时发 `egia_stopping`，停止接收提案，结算／检查点记录已知结果，再发一次 `egia_stopped`：`state:stopped` 使用 `level:info`；`state:stop_unconfirmed` 使用 `level:error`，确保未确认停止仍是可见故障。强杀可能没有终结事件。

## 映射和顺序

当前 `MissionQueue` 记录 `mission_started`、终结转换和检查点恢复；新投影仅在持久 running 状态后发 `egia_mission_started`，持久终结检查点后发 `egia_mission_terminal`，不确定的恢复 barrier 发 `egia_recovery_required`。`SafetyGate` 和 `EmergencyLatch` 将安全拒绝／锁存转换映射为不含原语名或参数的事件。EOS link 只有取得持久 EOS operation ID 后发 `egia_operation_submitted`；终结结果需要匹配 EOS 收据；超时／断连／缺失收据变为 `egia_operation_unknown`。这是现有归属状态边界；A03 后续增加阶段到操作关联。提案或图连线本身不能发 submitted 或 terminal。`correlation_id` 仅在已准入请求 ID 有效时保留，否则 null。必需 ID 缺失或格式错误时，只发安全的归属私有诊断，不伪造共享事件。

只投影 `egia-role-policy.json` 固定的类型化字段和消息。写入前验证枚举、ID、正数、规范且已准入的 EGIA 节点指针、整行长度和 UTC 时间。丢弃不安全候选字段，不把任意对象转为文本。字段专属安全原因如 `internal` 可保留可处理类别而不泄露异常。`message`、事件码和字段名不能来自模型输出、配置、图值、原语参数或供应商回复。私有 replay 的现有递归脱敏仍有用，但共享事件需要更严格的允许清单。

归属方本地最多 256 键的单调表只对重复的非关键诊断在 60 秒内抑制，发出有界计数。启动、就绪、状态变化、首次警告／故障、锁存、未知结果、终结和安全停止失败绕过抑制。当前 `_RepeatSuppressor` 对 replay／文件使用 4,096 项策略；不能复用其输出作为共享事件流，因为其签名和 `repeats` 字段不同。表淘汰和关闭时刷新安全计数，不阻塞停止。共享写失败增加有界归属计数；该失败或 IsOS 收集器故障均不改变 EOS 致动状态。

## 验证计划

[P06 夹具](../../../ei/tests/fixtures/jsonl_events/README_cn.md)冻结精确机器目录、字节上限、运行绑定、脱敏和有序生命周期案例。实现测试使用假时钟、捕获的 stderr、stub EOS 发现／结果及隔离 replay 目录；边界测试运行真实 IsOS 所选子进程 Supervisor，检查被接受的 `egia.log`／`events.log` 一致、首个故障和终结可见、收集器故障以及私有 replay 延续。在忽略的 `logs/test/<run-id>/` 保存完整 Ruff、pytest、Pyright、import、validate 和边界输出。此设计提交只提供契约证据，不代表运行时证据。

2026-11-01 设计检查：`python3 tests/fixtures/jsonl_events/check_cases.py`、`python3 -m py_compile tests/fixtures/jsonl_events/check_cases.py`、英文／中文本地链接和 JSON 示例检查、机器策略结构检查及 `git diff --check` 均通过。初始完整输出位于忽略的 `logs/test/20261101-p06-egia-design/`；停止级别修正输出位于 `logs/test/20261101-p06-egia-stop-correction/`。修正后的检查器报告 16 个精确事件码、45 个命名案例、四种停止状态／级别组合，以及原始 8,064/8,065 和存储 8,192/8,193 字节边界。

2026-11-01 实现证据：`uv run ruff check .`、`uv run pytest`、`uv run pyright`、`PYTHONPATH=src:libexec uv run lint-imports`、`./bin/islandctl validate` 和 `python3 tests/fixtures/jsonl_events/check_cases.py` 均通过。Pytest 包含封闭策略、托管绑定无效进程、严格托管 stderr／私有 replay、停止级别、脱敏、1000 次重复抑制和收集器故障测试。完整输出位于忽略的 `logs/test/20261101-p06-egia-implementation/`。所选 IsOS 子进程／收集器回放仍是跨归属集成门槛；这些本地测试不声称它已通过。

2026-11-01 实现审核修正：`SubmitOperation` 响应丢失时，EOS 可能已经准入运动，因此 EGIA 将物理结果视为未知，投影 `egia_operation_unknown` 和 `egia_recovery_required`，保留停止未确认状态，绝不重新提交运动。调用方命令 ID 在 EOS 发现前按共享不透明 ID 语法检查。托管模式中 EventSink 持有所选 stderr 管道的副本；操作系统文件描述符 2 重定向至 `/dev/null`，使原生写入不能绕过 JSONL 策略或持久化未脱敏秘密。831 项通过的测试包括 Submit 响应前 stub `UNAVAILABLE`、非法 ID 和原生 `os.write(2, ...)` 子进程。完整 Ruff、pytest、Pyright、import、validate 和夹具输出位于忽略的 `logs/test/20261101-p06-egia-review-fixes/`。

## A03 持久恢复与共享策略修订

封闭夹具 `tests/fixtures/jsonl_events/egia-role-policy.json` 的 schema 为 `egia-jsonl-role-policy-a03-v2`，共 24 个事件码。新增阶段、提案、准入、任务恢复、监测和本机重置记录只含审核过的标量字段。安全拒绝的抑制键包含关联 ID，故每个不同请求的首次拒绝仍可见；锁存、重置、准入、未知、终结和恢复记录不抑制。IsOS 必须镜像精确夹具后才能接收新码。

`OperationLedger` 在 EOS Submit 前原子预留 ID，传输不确定时保留待决条目。启动后只用同 ID 调用 `GetOperation`；匹配的终态收据结算检查点，缺失或不匹配的收据保持未知并阻止安全重置。`SafetyState` 在 estop 外发前将受影响 alias 和 EOS robot ID 集合写入仅所有者可读的原子文件并 fsync 目录。本机停止进程 CLI 使用与运行时同一把排他锁，写一次性授权；下次启动消费授权并两次检查所有 alias 后才持久解锁。

A03 进程边界测试保持单个 stub EOS 服务运行，让两个 EGIA 解释器共享私有状态根目录和固定的模拟 run 绑定。第一个进程预留检查点后在结算前退出；第二个进程执行正式的待决操作启动循环，完成完整发现并用保存的 ID 调用 GetOperation 后才结算。另一个缺失收据案例保留检查点和锁存，并拒绝一次性重置。该所有者测试不代表所选 IsOS 监管行为：R06 在 EGIA 子进程意外退出时停止整个部署。

A03 独立实现审查修正：`safety-history.json` 先于锁存 payload 写入，曾锁存而 payload 缺失时关闭准入；`operations-history.json` 和每个操作索引先于操作 payload 写入，缺失文件会被检测。锁存和一次性授权都绑定 episode ID 与所选图 revision；操作预留还保存 EOS robot ID 与图 revision，终态不可重写。运行锁取得后立即消费授权，配置、发现或任务检查点失败也不会留下可重用授权。托管默认启动不会在恢复前清除未完成任务。

审核修正证据（2026-11-02）：`uv run ruff check .`、`uv run pytest -q`（905 项通过）、`uv run pyright`（零错误）、`PYTHONPATH=src:libexec uv run lint-imports`、`./bin/islandctl validate` 和 `python3 tests/fixtures/jsonl_events/check_cases.py` 均通过。完整输出位于被忽略的 `logs/test/20261102-egia-a03-review-fix/`。测试覆盖 marker 先于数据写入的崩溃、即使其他操作仍在时的锁存／检查点缺失、episode 和修订漂移、终结结果不可改写、授权提前消费，以及托管默认检查点恢复。事件夹具字节保持冻结。
