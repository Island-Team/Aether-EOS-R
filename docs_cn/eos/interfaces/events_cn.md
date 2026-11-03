# Rust EOS JSONL 事件

[English](../../../docs/eos/interfaces/events.md) · [EOS 接口](README_cn.md) · [P06 任务](../../island/implementation-todo_cn.md)

IsOS 归属部署日志库、轮转、游标和管理器会话的启动前失败。每个受管理的 Rust EOS 进程归属自己产生的事件。本契约覆盖 `aether-runtime`、`aether-http-bridge` 和 `aether-config-server`；`aether-profile` setup 与 `--capabilities` 是有界探针，不是常驻模块。Rust 生产者现输出本 P06 封装；IsOS 收集及首次使用验收属于另一个集成步骤。可选 C++ [`eos::lib::LogEvent` 旧设计](../design/lib_cn.md)使用不同字段和路径，其附件与 ActionList 审计不进入 0.x Rust 事件流。

## 交接与传输

IsOS 仅在启动前包、图、读取器和生成配置检查全部通过后分配部署 `run_id`，用 `--run-id <id>` 把同一值传给每个受管理 EOS 子进程。ID 长 1–64 个 ASCII 字母、数字、`_` 或 `-`；无效或重复参数在准入前失败。没有 `--run-id` 的独立 Rust EOS 子进程生成同格式的本地 ID。EOS 向 stderr 逐行写 UTF-8 JSON 对象；事件初始化后不混入任意诊断文本。Runtime stdout 保留既有 ready／控制协议；能力和 setup 探针 stdout 保留既有 JSON 结果。IsOS 解析子进程流、将 `source`、`core` 和 `run_id` 绑定到所启动进程、拒绝冲突声明，只在规范化存储的记录中加入它观测到的子进程 `pid`，保留合法封装并汇总；不能根据日志行推断就绪。Runtime 为自身及 backend 生命周期发 `source:eos`，bridge 发 `source:bridge`，config server 发 `source:config`。原始 EOS 记录不含 `pid`；声称 `pid` 的原始记录会被拒绝。受管理启动时，IsOS 将 `--log-dir` 指向私有的 `data/logs/<deployment>/owner/eos/`，仅用于 MuJoCo 原始 `mujoco.log` 捕获。该文件与共享 JSONL 封装分离；原始 MuJoCo 诊断不作为 EOS 事件展示。EOS 不写顶层 `eos.log` 或 `events.log`。`eos.log`、`bridge.log`、`config.log` 及汇总 `events.log` 仅由 IsOS 写入。

IsOS 在启动前重新校验固定的 schema-2 MuJoCo 收据。带 `--backend-node` 的受管理启动还从该精确验证收据传入 `--backend-release-sha256 <digest>` 与 `--backend-payload-sha256 <digest>`。为保留独立／手动 EOS 启动路径，新参数是可选的一对；出现时两者必须同时存在，均为小写 64 位十六进制，且没有 backend 启动时拒绝。EOS 核对图 pin 和启动参数后，才以两个摘要发出 `eos_package_admitted`。缺少此对时发出仅含精确 pin、无摘要声明的 `eos_package_provenance_unavailable`；该警告不改变既有独立启动准入。摘要是 IsOS 提供的来源，不代表 EOS 独立验证包签名。EOS 不记录本机包路径或原始收据字节。IsOS 包／Apply 启动前失败使用管理器会话的 `run_id` 和可选请求 `correlation_id`；尚未启动的 EOS 不发事件。

## 封装

合法原始 EOS 事件包含以下字段。P06 收集器拒绝未知顶层键；未来扩展需要修订共享契约。事件不包含负载、附件或原始命令参数。生产者一行编码后的字节数（含换行）不超过 8064，为 IsOS 添加观测到的 `pid` 预留 128 字节，使规范化存储行不超过 8192 字节；序列化转义控制字符，不输出嵌入换行。

| 字段 | JSON 类型／单位 | 用途、缺失与校验 |
| --- | --- | --- |
| `timestamp` | 字符串／UTC 时间 | 必需，RFC3339、`Z` 结尾、毫秒精度。墙钟只供展示；单调时间用于抑制与期限。 |
| `level` | 字符串／— | 必需，`trace`、`debug`、`info`、`warn` 或 `error`；默认发送门限为 `info`，行动性警告和终态失败不能被隐藏。 |
| `source` | 字符串／— | 必需，`eos`、`bridge` 或 `config`，对应 IsOS 来源文件。MuJoCo 原始子进程输出保存在独立私有文件中，不属于本封装。 |
| `core` | 字符串／— | 必需，固定为 `eos`。 |
| `node_path` | 字符串或 null／— | 保存图中的 JSON Pointer `/cores/eos/layers/<layer>/groups/<group>/nodes/<node>`，最多 512 UTF-8 字节；进程级事件为 null。EOS 将经验证的内部四段路径 `eos/<layer>/<group>/<node>` 展开，每段中的 `~` 转义为 `~0`、`/` 转义为 `~1`；不发内部短路径或文件系统路径。当前 `valid_segment` 只允许 ASCII 字母、数字、`_`、`-`，含 `~` 或 `/` 的节点 ID 在图准入时被拒绝；转义规则仅约束防御性编码器。 |
| `event_code` | 字符串／— | 稳定的小写 snake_case ID，1–64 ASCII 字节；以下生命周期代码保留。 |
| `run_id` | 字符串／— | 必需，IsOS 提供的部署 ID，或独立启动的本地 ID；1–64 个 ASCII 字母、数字、`_`、`-`。 |
| `correlation_id` | 字符串或 null／— | 可选操作／请求 ID，与 `run_id` 同格式和上限；启动与主动健康变化时为 null。 |
| `message` | 字符串／— | 必需，共享封装上限 8192 UTF-8 字节；EOS 使用最多 256 字节的固定人工可读模板，使整行符合生产者的 8064 字节上限。不插入错误链、路径、负载、环境值或请求体。 |
| `fields` | 对象／— | 必需，最多 12 个白名单操作标量，序列化 UTF-8 字节数最多 4096；共享键格式为 1–48 个 ASCII 字母、数字、`_` 或 `-`，EOS 当前只发下表精确键。字符串值最多 256 UTF-8 字节且无控制字符；共享封装允许有限数字、布尔及 null，不允许数组／嵌套对象。 |

`fields` 由具体事件的构造器选择，不直接复制任意 `serde_json::Value`、`Debug`、stderr 或 tracing span。当前键还须遵循下列规则；缺少的键直接省略，不序列化为 null。不可信诊断映射为固定原因码和固定消息。

| `fields` 键 | JSON 类型／单位 | 校验与用途 |
| --- | --- | --- |
| `state` | 字符串／— | `starting`、`ready`、`failed`、`stopping`、`stopped` 之一；当前进程或节点状态转移。 |
| `stage` | 字符串／— | 进程失败边界：`options`、`profile`、`registry`、`adapter`、`journal`、`backend`、`listener`、`stop`、`sink` 之一。仅 `eos_skill_execution` 使用 `queued`、`running`、`completed` 或 `failed`。 |
| `reason_code` | 字符串／— | 进程码：`invalid_run_id`、`profile_invalid`、`registry_invalid`、`adapter_unavailable`、`journal_unavailable`、`backend_start_failed`、`child_exited`、`listener_bind_failed`、`stop_unconfirmed`、`event_sink_failed`、`other_failure`。`eos_skill_stop_unconfirmed` 使用 `skill_stop_unconfirmed`；`eos_skill_execution` 使用宿主固定的 `worker.*` 代码，最多 128 个小写 ASCII 字母、数字、`.`、`_` 或 `-`；guest 文本和 `worker_protocol_io`、`stage_unavailable` 等内部 `HostError` 代码映射为固定公开代码。绝不记录 guest 原文。 |
| `package_id`、`package_version` | 字符串／— | 精确已准入图 pin，分别至多 96 与 64 个 ASCII 字节；不能是本机路径。 |
| `release_sha256`、`payload_sha256` | 字符串／SHA-256 | IsOS 验证交接的摘要，恰为 64 个小写十六进制字符；必须成对出现。 |
| `node_count` | 整数／节点 | 已准入节点数，`0..4294967295`。 |
| `execution_id` | 字符串／— | 仅用于 `eos_skill_execution`：宿主发出的 `ex-` 身份，最多 80 个小写 ASCII 字母、数字、`.`、`_` 或 `-`。 |
| `worker_reaped` | 布尔或 null／— | 仅用于 `eos_skill_execution`：实际 worker 是否已回收；尚无回收观测的排队／运行转换为 null。 |
| `worker_exit_code` | 整数或 null／进程退出码 | 仅用于 `eos_skill_execution`：可用时记录已观测 worker 退出码；不使用 guest 自报状态。 |
| `suppressed_count` | 整数／事件 | 自上次汇总后省略的重复事件数，在 `4294967295` 饱和。 |

任何字段或消息均不得含密钥、凭据、令牌、环境表、原始图像、提示词、原语参数、提供方文本、套接字路径、场景路径、附件或媒体字节。0.x 的 Rust 事件流不创建附件。

验证 backend 包记录：

```json
{"timestamp":"2026-11-01T08:00:00.100Z","level":"info","source":"eos","core":"eos","node_path":"/cores/eos/layers/process/groups/mujoco/nodes/sim","event_code":"eos_package_admitted","run_id":"run_7","correlation_id":null,"message":"EOS backend package admitted","fields":{"package_id":"island/mujoco-sim","package_version":"0.1.1","release_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","payload_sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}}
```

进程就绪记录：

```json
{"timestamp":"2026-11-01T08:00:00.123Z","level":"info","source":"eos","core":"eos","node_path":null,"event_code":"eos_ready","run_id":"run_7","correlation_id":null,"message":"EOS runtime ready","fields":{"state":"ready","node_count":3}}
```

终态失败示例：

```json
{"timestamp":"2026-11-01T08:01:00.000Z","level":"error","source":"eos","core":"eos","node_path":"/cores/eos/layers/process/groups/mujoco/nodes/sim","event_code":"eos_backend_lost","run_id":"run_7","correlation_id":null,"message":"EOS backend child exited","fields":{"state":"failed","reason_code":"child_exited"}}
```

## Q01 浏览器命令关联——EOS 生产端已实现

回环 HTTP 桥接器在私有 `primitive_typed` 返回有效结果，以及经授权的 `result` 后续查询观察到新的终态时，生成脱敏的 `bridge_primitive_result` 诊断事件。IsOS 对选定桥接子进程的接纳和 P07 回放属于待完成的归属方工作。事件不证明物理运动完成；Dispatcher 日志和受管停止仍是权威。事件固定 `message: "EOS browser primitive result"`、`node_path: null`。`Failed`、`Rejected`、`OutcomeUnknown` 的 `level` 为 `warn`，其余为 `info`。`fields` 恰含 `status`（`Completed`、`Failed`、`Cancelled`、`Rejected`、`InProgress` 或 `OutcomeUnknown`）和 `phase`（`submit` 或 `followup`）。无效或矛盾的私有响应不产生成果事件。事件不得包含参数、绑定、目标、授权凭据、原始原因、帧或响应正文。

浏览器公开 `command_id` 仍接受 1–128 字节 UTF-8。ID 若由 1–64 个 ASCII 字母、数字、`_` 或 `-` 组成且不以 `h_` 开头，`correlation_id` 就是原 ID。其他有效 ID 的关联值为 `h_` 加 `SHA-256("eos-browser-command-v1\0" || UTF8(command_id))` 的前 62 个小写十六进制字符。保留 `h_` 前缀，避免原 ID 冒充派生令牌。IsosUI 生成的 `ui-<UUID>` 走原 ID 分支；其他客户端可从自己的有效 ID 算出 P07 筛选令牌。桥接器绝不把任意 UTF-8 复制到该字段。事件写入失败不改变命令结果或阻塞停止。有界线程安全命令状态缓存记录提交结果和首次观察到的终态，并抑制重复的 `InProgress` 轮询及终态读取。缓存逐出或桥接器重启可重复诊断行，但不改变 Dispatcher 日志。P07 在轮转后保留首个就绪/失败锚点；一般命令事件遵循有界保留和可见缺口规则。

```text
{"timestamp":"2026-11-02T08:00:00.100Z","level":"info","source":"bridge","core":"eos","node_path":null,"event_code":"bridge_primitive_result","run_id":"r_demo","correlation_id":"ui-018f","message":"EOS browser primitive result","fields":{"status":"Completed","phase":"followup"}}
```

非阻塞写入器发生错误后，该桥接进程的事件流永久失效；同一进程无法重试诊断行，缺少关联日志将使完整 Q01 验证失败。新桥接进程拥有新的写入器，可以生成之后的事件，但不能伪造缺失的旧行。

## 生命周期与抑制

| 模块 | 启动、就绪、终态及停止代码 |
| --- | --- |
| Runtime | `eos_starting`、`eos_ready`、`eos_start_failed`、`eos_stopping`、`eos_stopped` |
| HTTP bridge | `bridge_starting`、`bridge_ready`、`bridge_start_failed`、`bridge_stopping`、`bridge_stopped` |
| Config server | `config_starting`、`config_ready`、`config_start_failed`、`config_stopping`、`config_stopped` |
| Runtime backend | `eos_package_admitted`、`eos_package_provenance_unavailable`、`eos_backend_ready`、`eos_backend_lost`、`eos_stop_unconfirmed` |
| Runtime 原生技能宿主 | `eos_skill_execution`、`eos_skill_stop_unconfirmed` |

`eos_skill_execution` 使用 `source:eos`、`core:eos`、`node_path:null`；排队／运行／完成的 `level` 是 `info`，失败是 `error`。固定消息为 `EOS native skill execution lifecycle`。它携带部署 `run_id`、由公开请求 ID 哈希生成的宿主 `skill_` correlation ID，以及安全的 `execution_id`、`package_id`、`package_version`、`stage`、`reason_code`、`worker_reaped`、`worker_exit_code` 字段。只有确认 worker 回收后，`completed` 才使用 `worker.succeeded`；即使 guest 返回成功，未回收的 worker 也必须产生 `failed`、`worker.reap_failed` 且不返回结果。软件包 pin 由父进程选择和验证，guest 不能填写；`0.1.0+build.1` 等 P01 构建元数据版本有效。`eos_skill_stop_unconfirmed` 使用 `source:eos`、`core:eos`、`node_path:null`、`level:error`、固定消息 `EOS native skill stop unconfirmed`，字段为 `{"state":"failed","reason_code":"skill_stop_unconfirmed"}`。提供关闭 correlation ID 时使用该值，否则为 null。它覆盖 worker 尚未回收或宿主停止期限届满时 Dispatcher 取消仍未完成的情形。两者均不宣称执行器物理停止已确认；Dispatcher journal 是权威。准入与取消行为见 [R04 原生宿主 ABI](skills/native/host-abi_cn.md)。IsOS 现已在其封闭的子进程事件策略中登记这两个精确代码。[R04 首次使用指南](../develop/host-skills_cn.md#r04-精确合并版本可检查退休验收2026-11-02)记录了精确合并版本的保留段重放；该轮转由测试程序触发，而非自动大小阈值。

每个受管理进程在合法 run ID 和事件流准备好后，恰发一次 `eos_starting`、`bridge_starting` 或 `config_starting`。Runtime 只有在图、适配器、backend 与控制 socket 全部准入后才发 `eos_ready`。Bridge／config 在已校验的本地监听端点绑定后发对应 `*_ready`；这些记录只表示本地传输就绪，不声明上游 EOS 或整套部署就绪。启动失败发一次 `*_start_failed` 终态记录。Runtime backend 状态转移按适用情况发 `eos_package_admitted` 或 `eos_package_provenance_unavailable`，随后发 `eos_backend_ready`、观察到直接子进程退出时的 `eos_backend_lost`，以及 `eos_stop_unconfirmed`；R05 以后才覆盖仍存活子进程的 socket 故障检测。正常退出先发 `*_stopping`，再发一次 `*_stopped`；`*_stopped` 仅表示进程退出完成，发生 `eos_stop_unconfirmed` 时绝不代表实体停止已确认。突然被杀可能没有终态行，由 IsOS 监护状态报告。失败启动绝不伪造 `*_ready`。Dispatcher journal 是命令结果及未知释放的权威；日志事件只是诊断证据，不是命令回执。

状态转移、首次失败、就绪、终态与安全停止失败不被速率抑制。相同的非终态重复 `warn`／`error` 以 `(source,event_code,node_path,reason_code)` 为键立即发一次，持续出现时至少每五个单调秒再发一次并携带有界 `suppressed_count`；`debug` 重复可用同一窗口汇总。抑制表至多保留 256 个键，多余键归入一条 `eos_events_suppressed` 计数记录。图像帧、轮询、采样与控制周期不逐项产生事件。stderr 写入失败不能被当成就绪或确认的实体安全停止；进程状态和 Dispatcher 结果仍是权威。

[P06 夹具](../../../tests/fixtures/jsonl_events/README.md)冻结精确封装、来源、脱敏、生命周期与抑制边界。[运行事件设计](../design/runtime/events_cn.md)把契约映射到当前 Rust 入口。IsOS P07 归属保留期、游标恢复及来源汇总。
