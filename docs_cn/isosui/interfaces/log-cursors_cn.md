# 当前运行日志导航——P07 UI 契约

[English](../../../docs/isosui/interfaces/log-cursors.md) · [工作台设计](../design-log-cursors_cn.md) · [首次使用](../usage_cn.md)

**UI 切片已实现；需要 IsOS P07 服务端支持。** IsOS 拥有管理器会话 ID、部署运行 ID、接收事件顺序、保留策略、首事件锚点、不透明游标、轮转缺口和实时状态。IsosUI 只呈现这些所有者值。浏览器本地控制台是独立诊断来源，不能声明 IsOS 运行、事件、PID 或就绪。P07 面板显式请求 `view=current`，在筛选器上方固定归属锚点，并将浏览器日志分开。旧版 IsOS 响应会显示明确的兼容提示。

## 选择与顺序

打开 Home 日志时先读取 IsOS 状态。可选的 `GET /api/isos/v1/logs?view=current` 响应以 `scope` 提供当前管理器会话或部署运行。任何运行准入前，`scope.kind` 为 `manager_session`，锚点等待跨所有来源的首条类型化包或 Apply 故障；Run/Restart 启动前的包故障合格，只有 Validate 的故障不合格。一旦运行准入，IsOS 将 `last_run_id` 持久保留为 `deployment_run` 范围，穿越停止、故障、PID 消失、活动修订清空和管理器重启；只有下次准入运行才替换。P07 没有清除端点。它的锚点等待跨所有来源的首条保留就绪或故障。不能从 PID、活动修订或最新日志行推断范围。服务端锚点按 IsOS 接收事件的序号排序，不能按客户端时间戳排序。`anchor.state` 为 `pending` 时，`anchor.event` 为 null 且 `lines` 为空；用绑定运行的游标继续轮询，并显示等待状态。`unavailable` 与 `log_anchor_unavailable` 表示首事件无法持久化或恢复；应显示明确的丢失提示，而非永远等待。找到后，在任何来源、级别、节点或关联筛选前显示锚点；筛选后的详情 `lines` 严格从锚点之后开始，操作员缩小列表时仍将其固定。管理器会话中的启动前失败不能被改标成旧的活动运行。正常的筛选后 `lines:[]` 响应若推进 `next_cursor` 且没有诊断，仅表示所扫描范围内没有匹配详情事件；显示无匹配状态并继续轮询。只有 `anchor.state:unavailable` 或类型化丢失诊断才显示不可用或缺口警报。不能从未筛选的页面猜造首事件。

默认详情流是 `scope` 返回的**当前**会话或运行。UI 请求 `view=current`，把游标视为不透明值，不解码也不跨会话或部署保存。IsOS 将游标绑定到 `scope`、查询和保留代际；运行改变时返回新范围及 `log_run_changed`，游标过期时在同一范围内以 `log_cursor_expired` 重定位。UI 在展示行前将响应范围与上一次范围比较，变化时重新获取状态。P07 没有历史运行选择器；新运行范围会产生可见边界和新锚点请求。

可选响应包含 `scope:{kind:manager_session|deployment_run,run_id,active_revision}`、未筛选的 `anchor:{kind:first_prelaunch_failure|first_ready_or_failure,state:pending|found|unavailable,event}`、`lines`、`next_cursor`、`rotated`，以及带可空数量的 `log_run_changed`、`log_cursor_expired`、`log_collector_gap` 或 `log_anchor_unavailable` 类型化 `diagnostics`。`source`、`level`、`node_path`、`correlation_id` 只筛选详情 `lines`；`anchor` 不变。`limit` 为 1–200，默认 100；IsOS 每页最多扫描 64 KiB 已保留日志字节，筛选后空页仍可推进游标。`GET /api/isos/v1/process-status` 新增可空 `run_id` 和 `run_id_state:none|known|unavailable`：none 是尚无准入运行的持久证据，known 指向末次准入运行，unavailable 表示 IsOS 无法证明该身份。状态仍是实时权威。身份不可用时，`view=current` 在 `/scope` 返回 HTTP 503 `log_scope_unavailable`；保留旧行并标为过期，显示可访问错误，绝不回退管理器范围。没有 `view=current` 的旧路由保持兼容。仅提供 `{lines,next_cursor,rotated}` 的旧版 IsOS 可显示保留行及兼容提示，但 UI 不能把首条显示行称为首事件，也不能静默跨运行边界。

| UI 输入或所有者字段 | JSON 类型／单位 | UI 校验与用途 |
| --- | --- | --- |
| `view` | 固定 `current`／— | P07 必需；其他值由 IsOS 报错。 |
| `source`、`level` | 来源 ID 或 `all`；级别枚举／— | 默认 `all` 和 `info`；仅筛选详情，不能用于选锚点。 |
| `node_path` | 规范保存图节点 JSON Pointer／— | 可选精确筛选，最多 512 UTF-8 字节；请求前拒绝错误或成员指针。拒绝时在 `/query/node_path` 返回 HTTP 400 `log_node_path_invalid`。 |
| `correlation_id` | ASCII ID／— | 可选精确筛选，1–64 个字母、数字、`_` 或 `-`；不是自由文本搜索。拒绝时在 `/query/correlation_id` 返回 HTTP 400 `log_correlation_id_invalid`。 |
| `limit` | 整数／事件 | 默认 100，范围 1–200；请求前限制 UI 输入。 |
| `cursor`、`next_cursor` | 不透明 ASCII 字符串／位置 | 输入游标最多 512 字节；保持相同查询并原样传回返回值。不解析，也不跨运行保存；筛选变化时重置。复用筛选不匹配的游标时返回 HTTP 400 `log_cursor_filter_mismatch`，路径为 `/query/cursor`；损坏或伪造的游标在同一路径返回 `log_cursor_invalid`。固定诊断不回显令牌或筛选值。 |
| `scope` | 对象 `{kind,run_id,active_revision}`／— | 必需；kind 为管理器或部署，ID 为 1–64 ASCII 字节，活动修订为摘要或 null。身份只选日志历史，不代表实时健康。 |
| `process-status.run_id`、`run_id_state` | 字符串或 null；枚举／运行身份 | `run_id_state` 为 `none`、`known` 或 `unavailable`；仅 known 有 1–64 ASCII 字节的末次准入运行 ID。none 允许管理器范围；unavailable 阻止当前运行声明。 |
| `anchor` | 对象 `{kind,state,event}`／— | 必需；待定／不可用的 event 为 null，找到时是完整脱敏 P06 事件。固定显示于全部筛选器上方。 |
| `lines`、`rotated` | 事件数组；布尔值／— | 至多 `limit` 条同范围且严格晚于锚点的事件；仅 `rotated` 不代表丢失。 |
| `diagnostics` | `{code,count}` 数组／丢失事件 | code 为上述四种稳定值；count 为非负整数或未知时 null。丢失或运行改变要显示持续可访问的提示。 |

## 行、错误与上限

每条已接收记录展示 `timestamp`（UTC 毫秒）、`level`、`source`、`core`、可选的保存图 `node_path`、稳定的 `event_code`、所有者 `run_id`、可选的 `correlation_id`、脱敏 `message` 以及可用时观测到的 `pid`。仅将字段呈现为文本，不渲染 HTML 或原始子进程 stderr。可在折叠区显示有界类型化 `fields`。旧记录缺少字段时标记为不可用，不推断。即使最新事件写着 ready，IsOS 状态仍是就绪和停止的权威。

同一时刻最多一次轮询；每次最多请求 200 行，IsOS 每页最多扫描 64 KiB，保留最多 1,000 条显示行；隐藏页面或关闭 Follow 时暂停，重复失败时重试间隔至少为 30 秒。短暂收集器故障时保留末次游标与行，以非颜色 `role=alert` 显示错误并重试。恢复时先重新核对状态和查询身份，再继续。游标过期或保留范围丢失时，显示可访问的缺口通知（若 IsOS 提供数量／范围则展示），然后从 IsOS 给出的同运行有界位置恢复。不能静默清除缺口、把旧行重新当成新行，也不能回退到全部运行。仅在用户要求时重置来源／节点／关联详情筛选；它们不遮盖固定的首事件上下文。新运行会清除旧详情行与游标，显示命名边界，然后从新的当前页面读取自身的首事件锚点。

IsOS 在启动任何子进程前持久发布新运行 ID 和待定锚点。若 process-control 在 `/run_id` 返回 HTTP 500 `log_scope_publish_failed`，显示类型化启动前错误，不声称子进程或就绪事件。不能假定旧运行仍为当前范围：不确定的 fsync 可能让新运行持久准入，尽管没有子进程启动。重新读取 `process-status` 和 `view=current`；若 IsOS 报告幸存的新运行，则显示其边界及所有者报告的待定或不可用锚点。若身份不可用，遵循上述 HTTP 503 处理。

操作员可以暂停、继续、跳转首事件并复制安全 ID。`aria-live=polite` 只播报一次新运行边界或缺口；流式记录不连续轰炸播报。空列表、无匹配事件、收集器不可用和保留缺口分别提供中英文文本。[P07 夹具](../../../ui/tests/fixtures/log_cursors/README_cn.md)冻结这些 UI 转移。
