# EGIA 共享部署事件

[English](../../../docs/ei/interfaces/events.md) · [IsOS 收集器契约](../../isos/interfaces/cores/events_cn.md) · [设计](../design/events.md) · [首次使用](../develop/build-and-run.md) · [P06 任务](../../island/implementation-todo_cn.md)

**P06 信封及 A03 封闭策略修订已实现。** EGIA 负责 `egia` 事件的语义和安全构造。IsOS 负责运行 ID 分配、所选子进程管道绑定、聚合、轮转和游标读取。所选托管 EGIA 进程向 stderr 为每个事件写一个严格 UTF-8 JSON 对象及 `\n`；stdout 和已有私有 replay 独立。只有所选直接子进程的 stderr 管道可在 IsOS 中建立 `source:egia`。监听端口、PID 或日志行本身不证明部署健康。

## 信封和身份

原始子进程行最多 **8,064 字节，包含换行**。EGIA 先脱敏，再在 UTF-8 边界缩短固定消息，直到整行符合上限。IsOS 添加观测到的 PID，存储行最多 8,192 字节。顶层字段如下；禁止生产者提供 `pid` 或扩展键。

| 字段 | JSON 类型／单位 | 用途、缺省、验证和兼容性 |
| --- | --- | --- |
| `timestamp` | RFC 3339 UTC 字符串／毫秒 | 必需的实际事件时间，保留毫秒和 UTC `Z`。 |
| `level` | `trace`、`debug`、`info`、`warn`、`error`／— | 必需级别；下述策略为每个 EGIA 事件码固定级别。 |
| `source` | 字符串／角色 ID | 必须为 `egia`；IsOS 将其绑定到所选直接子进程。 |
| `core` | 字符串／归属 ID | 必须为 `egia`。 |
| `node_path` | 规范 JSON Pointer 或 null／已保存图 | 可选，缺省为 null。非 null 值必须恰好结束于已准入 EGIA 节点，不能是成员路径；最多 512 UTF-8 字节。进程生命周期和发现事件必须为 null。 |
| `event_code` | 小写 ASCII 字符串／事件码 | 必需，1–64 字节；只发出[角色策略夹具](../../../ei/tests/fixtures/jsonl_events/egia-role-policy.json)中的精确码。新码须先经 EGIA 和 IsOS 策略审核。 |
| `run_id` | ASCII 字符串／运行 ID | 必需，1–64 个 `[A-Za-z0-9_-]`。托管 `ISOS_RUN_ID` 在启动前须匹配 `r[A-Za-z0-9_-]{1,63}`；缺失或错误时关闭准入，不发 EGIA 共享事件。独立运行生成 `l` 加 32 位小写 UUID 十六进制。独立进程拒绝意外的 `ISOS_RUN_ID`，不自行取得管理器权限。 |
| `correlation_id` | ASCII 字符串或 null／请求 ID | 可选，缺省 null；语法和长度同 `run_id`。仅承载已准入请求或操作关联，不能是用户文本。 |
| `message` | 字符串／UTF-8 字节 | 必需的策略固定文本；不得是异常字符串、提示、命令或供应商回复。仅为整行上限进一步缩短。 |
| `fields` | 对象／有界元数据 | 可选，缺省 `{}`。最多 12 个事件码允许的标量键，序列化后最多 4 KiB；键长 1–48 ASCII 字母、数字、`_` 或 `-`；字符串最多 256 UTF-8 字节且不含控制字符。夹具固定必需／可选字段及枚举域。 |

完成 EOS 发现和安全初始化后的示例：

```json
{"timestamp":"2026-11-01T10:00:02.000Z","level":"info","source":"egia","core":"egia","node_path":null,"event_code":"egia_ready","run_id":"r7-cde","correlation_id":null,"message":"EGIA ready","fields":{"generation":3}}
```

IsOS Supervisor 拒绝角色／运行不匹配、格式错误或超长行、未知事件码、非法节点、不安全字段、重复 JSON 键或原始 `pid`。托管绑定无效时，EGIA 将安全诊断留在私有日志；共享事件不得伪造部署运行 ID。事件流建立前崩溃由 IsOS 的子进程监督记录。

## 封闭角色策略

[机器角色策略](../../../ei/tests/fixtures/jsonl_events/egia-role-policy.json)是精确事件码、消息、级别、节点规则、类型化字段和枚举值的规范清单。夹具中以 `?` 结尾的字段名表示可选，线上的键不含 `?`。24 个事件码如下：

| 类别 | 事件码 | 发出和抑制规则 |
| --- | --- | --- |
| 生命周期 | `egia_starting`、`egia_ready`、`egia_start_failed`、`egia_stopping`、`egia_stopped` | 配置／运行绑定／事件 sink 有效后一次 starting；完整 EOS 发现、模型绑定和安全门准备后才一次 ready；一次终结启动失败或停止结果。`egia_stopped` 在 `state:stopped` 时级别为 `info`，在 `state:stop_unconfirmed` 时为 `error`；相反组合无效。永不抑制。 |
| 发现 | `egia_discovery_lost`、`egia_discovery_restored` | 仅在完整发现代际实际丢失或恢复时发出，不按轮询发出。首次转换不抑制。 |
| 任务 | `egia_mission_started`、`egia_mission_terminal`、`egia_mission_restored` | 持久状态转换后发出。终结结果不抑制；`unknown` 不代表物理成功。 |
| 阶段与准入 | `egia_stage_transition`、`egia_proposal_created`、`egia_admission_decided` | 类型化阶段接收、门前提案和持久预留后的准入使用已验证的操作关联 ID。 |
| 安全 | `egia_safety_rejected`、`egia_safety_latched`、`egia_safety_reset_requested`、`egia_safety_reset_completed`、`egia_safety_reset_refused` | 首次安全拒绝和锁存立即发出。重复的非关键相同拒绝可汇总；锁存不抑制。 |
| 监测 | `egia_monitor_episode` | 只在故障及恢复转换时发出，不按心跳重复。 |
| EOS 操作 | `egia_operation_submitted`、`egia_operation_terminal`、`egia_operation_unknown` | EGIA 将已验证的操作 ID 作为 EOS 的持久幂等键发送。提交仅表示已确认的准入；终结需要 EOS 收据。若 Submit 响应丢失，EGIA 无法确定 EOS 是否已准入运动：发出 unknown 和 recovery required、标记停止未确认，且绝不重放运动。终结和 unknown 不抑制。 |
| 恢复 | `egia_recovery_required` | 检查点／重新发现或未知动作需要核对时发出；永不抑制。 |
| 有界重复 | `egia_events_suppressed` | 对重复的非关键诊断报告饱和正整数 `suppressed_count`，不得声称丢失的安全或终结事件已记录。 |

`generation` 是正的 EOS 发现代际，不能单独证明就绪。`mission_id` 和 `operation_id` 为匹配 `[A-Za-z0-9_-]{1,64}` 的不透明 ID。EGIA 在任何 EOS 发现或提交前按此语法验证调用方提供的命令 ID；非法 ID 以固定 `invalid operation ID` 失败，不调用致动器。原因和结果为夹具中的封闭枚举；未知内部故障映射到审核过的通用码，不复制任意异常。A03 夹具版本为 `egia-jsonl-role-policy-a03-v2`；IsOS 必须镜像这一精确策略后才能接受所选子进程的新事件码。非空 EGIA `node_path` 必须匹配已准入图节点；图边不成为致动器路径。

## 隔离、脱敏和丢失

已有 `var/lib/egia/egia.jsonl` 仍是 EGIA 的轮转归属记录（32 MiB、五个备份）；人类诊断仍在 `var/log/egia`。它们使用现有脱敏链，但不逐字复制到 IsOS。托管 stderr 只写封闭共享信封。EGIA 为事件 sink 复制所选 stderr 管道，然后将进程文件描述符 2 重定向至 `/dev/null`：原生写入、警告和回溯既不能污染共享管道，也不会持久化未脱敏字节。原始提示、转录、媒体、模型／供应商正文、原语参数／结果、URL、文件系统路径、凭据、环境变量名／值和异常链不得进入 `message` 或 `fields`。先验证 ID；不能靠截短任意文本使字段合法。

首次就绪、状态转换、警告、失败和终结记录立即发出。重复的相同非关键诊断按 `(run_id,event_code,node_path,reason_code,correlation_id)` 建键，使用单调 60 秒窗口和最多 256 项的表，在窗口／终结刷新时发出计数 `egia_events_suppressed`；计数在 `4294967295` 饱和。私有 replay 可保留更多已脱敏细节。共享 stderr 写失败记入有界私有计数，不得阻塞安全停止或 EOS 结果处理。EGIA 不能在收集器丢失后伪造就绪或终结确认。IsOS 状态和 EOS Dispatcher 收据仍是权威。


生产者位于 `src/egia/lib/shared_events.py`，审核过的事件码和字段表位于 `shared_event_policy.py`。`egia --managed` 在配置日志前验证 `ISOS_RUN_ID`；绑定无效时只写固定私有诊断并退出。所选托管进程保留私有 replay 和文件日志，stderr 仅供此精确 JSONL 投影使用。归属调用点发出启动、完整发现后的就绪、停止、任务检查点、安全、EOS 操作和恢复转换。就绪后的突发故障由 IsOS 监督器观察，EGIA 不伪造停止确认。
