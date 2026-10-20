# lib — leaf utilities shared by all layers

`src/eos/lib/` is the bottom of the include graph: every module, including headers under `src/eos/include/eos/`, may use it, and it depends on no other EOS module — third-party headers only. It builds as one `eos_lib` target that every target under `src/` links publicly; `eos_lib` itself links nothing from this tree. Admission is review-gated by one question — *would every layer plausibly need this?* — so it stays small; anything domain-shaped (tasks, coordinates, contracts, device concepts) belongs in its own module. Simple-tier doc; sections appear only where the module needs them.

One extra admission rule is specific to EOS: anything callable from an RT band (L0, L1a) must be allocation-free and non-blocking after initialization. Utilities that cannot hold that guarantee (file-backed logging, TOML parsing, path resolution) are launch-time only — they run before the bands start and are never reachable from a periodic loop. Each section below states which side of that line it sits on.

## log

`src/eos/lib/log.h` — spdlog setup, the first utility initialized at launch. Launch-time only; sinks are synchronous so the emitting thread's correlation ID reaches the JSONL record — RT bands must not call these sinks from a periodic loop (a lock-free relay for the bands lands with the first band process).

一次启动独占 `var/log/eos/runs/<launch-id>/`；各服务直接写入 `events.jsonl`、`main.log`、`runtime-<module>.log`、`ext-<module>.log` 与 `thirdparty.log`；launch 根目录包含 `launcher.log`。retained mode 创建唯一 ID，overwrite mode 复用 `current`，`var/log/eos/latest` 指向最新 run。HTTP request ID 经 adapter gRPC metadata 传播到 EOS 与 异步 skill work，同一 `correlation_id` 可关联跨进程事件。

north HTTP 预留 logger route `north-http.audit` → `ext-north-http-audit.log` 与 `north-http.client` → `ext-north-http-client.log`；client 当前不产生 event。`var/lib` 下的 durable workflow 与 ActionList journal 是 application data，不是 logging sink，因此不受 `LogConfig` overwrite、rotation、deduplication 或 attachment policy 影响。

`LogEvent` 记录稳定的 snake_case 事件名和类型化 metadata。JSONL 加入共享的 `launch_id` 与进程特有的 `run_id`。payload 不能作为日志 metadata；启用附件后写入扁平的 `var/log/eos/runs/<launch-id>/attachments/`，也可通过 `var/log/eos/latest/attachments/` 访问。文件使用 category prefix，例如 `asr-<uuid>.wav`、`tts-<uuid>.<ext>` 与 `vision-<uuid>.jpg`，不存在 `asr/`、`tts/` 或 `vision/` subdirectory。structured attachment metadata 包含 `category` 与 `relative_path`。`overwrite = true` 在写入新输出前清空整个 `runs/current` launch（日志和附件），不影响 retained run 或 `var/lib` 下的 durable journal。

File sinks suppress identical structured events inside `[log] dedup_window_s` when `[log] dedup` is enabled; the next emitted occurrence reports the accumulated repeat count. Deduplication retains at most 4,096 recent message identities per file sink, so unique correlation and command IDs cannot grow logging memory without bound. The console remains unsuppressed. Logging is synchronous: periodic and real-time-adjacent code reports state transitions at a non-real-time boundary and never logs every tick, frame, or unchanged poll.

API:

- `eos::lib::ConfigureLogging()` / `ConfigureLogging(LogConfig)` — 使用 `var/log/eos/runs/<launch-id>/eosd/` 的 standalone 默认配置。
- `eos::lib::ConfigureLogging(log_root, service, LogConfig)` — 进程入口首选形式；校验 service 路径组件，并创建隔离的路由目录和 `events.jsonl`。`rotation_size_bytes` 与 `rotation_files` 默认分别为 64 MiB 和五份历史文件；旧的最终目录 overload 保留给测试和嵌入式工具。
- `eos::lib::GetLogger(name)` — returns a `std::shared_ptr<spdlog::logger>` for the caller; safe to call before `ConfigureLogging` (falls back to a stderr-only logger).
- `eos::lib::LogEvent(logger, level, event, fields)` / `LogOnce(logger, level, key, event, fields)` — emit sanitized structured metadata normally or once per process-wide logger/key pair.
- `eos::lib::SetStderrLevel(level)` — adjusts the console threshold at runtime; the JSONL file keeps every event. The supervisor drops the console to WARNING once the bands report ready, so INFO appears only during launch and shutdown.
- `eos::lib::CorrelationScope(id)` / `eos::lib::CurrentCorrelationId()` — 为调用线程绑定 correlation ID 的 RAII 工具；嵌套 guard 会恢复外层 ID，空 ID 会生成 UUID4 风格 ID。`IsValidRequestId` 与 `ResolveRequestId` 为 HTTP 和 gRPC transport 提供统一的 1–96 byte token 策略，并在 metadata 缺失时生成 ID。

## paths

`src/eos/lib/paths.h` 在启动时一次性解析完整且不可变的路径集合。`ResolveEtcRoot` 依次使用 `--etc`、`EOS_ETC` 和 `ISLAND_ETC/eos`；`ResolveContractsRoot` 依次使用 `--contracts`、`EOS_CONTRACTS` 和 `ISLAND_SHARE`。`ResolveLayoutPaths` 还通过对应的 `ISLAND_*` root 解析 state、cache、log、runtime 和 temporary role，并且只创建这些可变目录。`EOS_VAR` 保留为兼容 root。显式参数和环境变量提供的 root 必须是绝对路径，因此行为不依赖当前工作目录；缺少 root 时启动会给出可操作的拒绝信息。Runtime initialization 将解析后的 EOS log role 导出为 `ISLAND_LOG_DIR`，供链接的适配器写入诊断日志。

## clock

`src/eos/lib/clock.h` — time source abstraction over `std::chrono::steady_clock` / `system_clock`. RT-safe: reads are `clock_gettime`, no allocation, no blocking.

One `eos::lib::Clock` interface with two implementations: `SystemClock` for production and `ManualClock` for tests, so freshness checks, watchdog deadlines, and snapshot timestamps are deterministic. Modules take a `Clock&` at construction; none call `std::chrono::*::now()` directly. The monotonic timestamps in the chain's $(v, t)$ slots come from this clock.

## slot

`src/eos/lib/slot.h` 为一个生产者和一个消费者提供无等待的数据交接。实现使用三个缓冲区：双方各持有独占缓冲区，并通过一个原子中间缓冲区交换所有权。`T` 必须可平凡复制，大小符合所属层的上限；不能通过该边界传递持有堆内存的对象。最新样本超过 `fresh_budget` 时，`Read` 返回空值，使消费者进入降级行为，而非把旧值当作有效观测。当前 body 快照仍按需构建；`Slot` 可用于未来固定频率交接。

## executor

`src/eos/lib/executor.h` 定义有界 FIFO worker pool。正常销毁会排空已接受任务并等待 worker。若某个任务销毁所属 pool，待处理任务被取消，worker 脱离以避免任务等待自身；已运行任务不会中断。内部状态保留到这些 worker 结束。`Shutdown` 只在首次调用时选择模式；pool worker 调用 `Join` 会在开始关闭前抛出异常。

Planned sibling: a TOML binding loader over toml++ (launch-time, naming the resolved file in every error) when the second config-reading module appears.

**Testing:** `log_test.cpp` 覆盖进程隔离、轮转、retained/current 模式、结构化上下文、request ID、脱敏、去重与附件。成功启动会写入 `var/log/eos/latest/eosd/events.jsonl`。
