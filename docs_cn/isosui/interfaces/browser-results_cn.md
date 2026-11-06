# 浏览器原语结果客户端——R07 UI 契约

[English](../../../docs/isosui/interfaces/browser-results.md) · [设计](../design-browser-results_cn.md) · [首次使用](../usage_cn.md) · EOS 所有者契约：`Island-eos/docs_cn/eos/interfaces/control_cn.md` 中的 R07 章节

**R07 已合并，并完成受管理模拟底盘范围的验证。** IsosUI [#37](../../island/retired-repository-references_cn.md#ref-bcef1a59120a) 与 [#38](../../island/retired-repository-references_cn.md#ref-9028ce737965) 实现最小类型化控制卡、受保护结果读取器及匹配运行 ID 的 Stop 显示。[共享 R07 验证记录](https://github.com/Island-Team/Island-eos/blob/refactor%28v2%29/runtime-main/docs/island/implementation-todo.md)证明一次操作员点击的模拟底盘命令以同一 ID 经历 `Sending`、`InProgress` 和持久化 `OutcomeUnknown`，产生一条日志记录，并显示同运行已确认的 Stop。MuJoCo 物理停止、U04 完整逐绑定控件及 M5 发布首次使用重放仍未完成。 IsOS 拥有保存图、活动修订、部署运行身份和 Stop。EOS 拥有活动绑定、准入、日志结果和实时状态。IsosUI 可以显示浏览器本地发送状态，但只有与命令关联的 EOS `DispatchResult` 能确定结果。图连线只声明一个绑定目标，不执行浏览器命令。MuJoCo 运动控件必须等 R06 物理停止验证后才能开放。

## 类型化边界

`src/services/eos/` 客户端只通过本地或托管构建配置的已验证回环地址调用 `POST http://127.0.0.1:16020/api/eos/v2/control`。请求使用 JSON、`credentials: omit`、无 Authorization 标头和现有四秒客户端超时。浏览器不发送 `authority_grant`、私有 `primitive_result` 或原始 Dispatcher 操作。桥接层校验 Host 与允许的 Origin，并自行添加运行范围内的操作员授权。R07 包含针对一个已验证活动绑定、由操作员点击触发的最小 `primitive_typed` 发送方及关联结果消费者；U04 后续补全每个绑定的目录、完整参数表单和节点卡。浏览器 `op: "result"` 只读，桥接层将其改写为私有操作。

最小发送方从**活动保存图**解析一个受支持的 `eos.dispatcher.primitive_base@v0.1` `base.velocity`／`body_velocity` 绑定，沿其唯一 `command` 连线找到兼容 base 目标，并在启用 Invoke 前检查 EOS 报告的目标就绪及硬件运动限制。安全准入的旧版 `@v1.0` 绑定可使用相同线路。R07 可仅呈现一个合格绑定，以及包含 `x_m_s`、`y_m_s`、`yaw_rad_s`、`valid_for_ms` 的小型类型化表单；必须显示绑定及目标路径、单位和边界。不能接受可编辑目标路径，也不能选择图草稿绑定。这个受支持表单足以通过 EOS 模拟适配器证明 `InProgress` 后续查询和 Stop，同时 R06 保持真实 MuJoCo 运动禁用。模拟测试本身不能开放真实运动；EOS 所有者报告的准入状态及 R06 停止证明必须共同允许目标。加载页面或选择绑定时不能自动发命令。

| 发送字段 | JSON 类型／单位 | 用途、默认值与校验 |
| --- | --- | --- |
| `op` | 固定值 `primitive_typed`／— | 必填且严格匹配；无默认值。 |
| `header.data_type_id`、`data_type_version`、`codec` | 字符串／注册身份 | 此 base 部分必须分别为 `base_movement`、`1.0`、`json_v1`；无默认值。 |
| `header.primitive_name`、`primitive_version` | 字符串／原语身份 | 必须与活动绑定完全一致；本部分支持 `base.velocity@1.0.0`。 |
| `header.source_path`、`target_path` | 字符串／规范图节点路径 | 必须分别为活动绑定及其唯一图连线 base 目标；不能从可编辑目标输入获取。 |
| `header.active_revision` | 字符串／保存图 SHA-256 | 必须为当前 IsOS 保存及活动修订，64 位小写十六进制；无默认值。 |
| `header.command_id` | 字符串／命令身份 | 每次调用新生成，UTF-8 长度 1–128 字节且满足 EOS Dispatcher 语法；超时后仍保留。 |
| `header.sequence` | 无符号整数／命令顺序 | 在当前运行内为所选绑定生成的正安全整数；无默认值。 |
| `header.deadline_monotonic_ns` | 无符号整数／纳秒 | EOS `time` 响应加上有界期限，绝不使用浏览器墙钟；发送前拒绝不安全 JSON 整数或溢出。 |
| `header.payload_length_bytes` | 无符号整数／UTF-8 字节 | 解析后的载荷紧凑 JSON 序列化精确长度，范围 1–16,384；不能以 JavaScript 字符串长度代替多字节长度。 |
| `payload` | 类型化对象／SI 单位 | 严格为 `body_velocity`，有限的 `x_m_s`、`y_m_s`、`yaw_rad_s` 不超过活动硬件限制，正整数 `valid_for_ms` 不超过租约上限。 |

公开发送请求恰好包含 `op`、`header`、`payload`；桥接层完成公开校验后添加私有授权。收到匹配的 EOS `{"ok":true,"result":...}` 响应时，使用与结果读取方相同的六状态解码。ID 缺失／不符、未知状态或损坏响应属于协议错误；传输故障表示提交结果不可用。浏览器不能重试可能已被接受的运动。本 R07 表单仅绑定模拟或通过 R06 验证的目标；U04 负责通用 base／arm 控件生成。

浏览器在公开请求中用 `TextEncoder(JSON.stringify(payload))` 计算 `payload_length_bytes`。EOS 桥接层验证所声明长度及解析后载荷的界限，再为私有 `serde_json` 运行时封装规范化长度。这使 JavaScript 与 Rust 紧凑数字写法不同的有效小 SI 数值仍可使用。

| 客户端字段 | JSON 类型／单位 | 用途、默认值与校验 |
| --- | --- | --- |
| `op` | 固定值 `result`／— | 必填且严格匹配；无默认值。 |
| `command_id` | 字符串／命令身份 | 必须与提交的 `primitive_typed` 命令头及 EOS 首次响应关联，UTF-8 长度为 1–128 字节；无默认值。 |
| `active_revision` | 字符串／保存图 SHA-256 | 必填 64 位小写十六进制，取自当前 IsOS 活动修订；每次读取时须与保存图修订相等；无默认值。 |
| `result.command_id` | 字符串／命令身份 | 必须等于本地跟踪的命令 ID；不符属于协议错误，不是命令结果。 |
| `result.status` | 枚举／— | 必须严格为 `Completed`、`Failed`、`Cancelled`、`Rejected`、`InProgress` 或 `OutcomeUnknown`；未知状态属于协议错误。 |
| `result.reason` | 字符串或 null／UTF-8 文本 | EOS 报告的解释，最多 512 UTF-8 字节；只当文本显示，不渲染 HTML，也不用于控制决策。 |

```json
{"op":"result","command_id":"move-1","active_revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}
```

结果读取服务在 fetch 前校验请求，只接受 `{"ok":true,"result":{"command_id":...,"status":...,"reason":...}}`，且 ID 相符、字段有界。EOS 将整个成功响应限制为 4 KiB。成功响应损坏、未知状态、ID 不符或响应过大均显示为协议错误，绝不能显示为 `Completed`。所有者固定错误仍是错误：请求无效（`400 invalid result request`）、Origin 或 Host 无效（`403`）、操作员授权不可用（`503 operator control unavailable`）、私有授权拒绝（`operator control grant required`）、修订过期（`stale active revision`）、其他运行／角色／入口／类型或不存在 ID 的统一不透明错误（`unknown command ID`）、有界结果不可用（`result unavailable`）以及 IPC 离线（`502 runtime unavailable`）。UI 不能根据 `unknown command ID` 猜测隐藏原因。

## 运行与修订围栏

首次类型化发送之前，读取 IsOS 图和 `process-status`，要求：图支持编辑、无未保存草稿、`state:running`、已知当前 `run_id`、64 位十六进制保存修订与图文档修订及活动修订相同、活动绑定严格且只有一个兼容目标、EOS 就绪且目标健康。每次结果读取前重新获取 IsOS 进程状态，要求已知运行 ID 和活动修订与发送时记录相同、`state:running`，且保存修订不变；绑定或健康变化不抹除已准入的命令。IsOS 无法证明运行身份、状态不可用、新运行或新活动修订出现时，停止自动读取，把旧命令标记为**本运行无法获取结果**。保留最后一个所有者状态和命令 ID 供排查，但不带入新运行，也不改成 `Completed`。草稿修改或图只读变化禁止新发送，不能为已有命令造出终态。

命令记录键为 `(run_id, active_revision, binding_path, command_id)`：`run_id` 是 IsOS 当前部署运行精确 ID，`active_revision` 是 64 位十六进制摘要，`binding_path` 是活动图节点路径，`command_id` 每次调用只生成一次。只有当前选中的记录能更新其可见状态。页面卸载、绑定选择变化、运行／修订变化或 Stop 时取消未完成的浏览器读取，并用代际标记忽略迟到响应。旧代响应不能覆盖新命令。不能在浏览器存储中把命令 ID 或结果保存为权威。只有 EOS 所有者事件属于捕获的运行与命令，且提供长度 1–64、仅含 ASCII 字母／数字／`_`／`-` 的有效 IsOS 关联 ID，当前运行日志入口才能预填 `correlation_id`；否则单独显示命令 ID，打开无筛选的当前运行日志。绝不能假设 `command_id` 等于 `correlation_id`。

## 可见状态与 Stop

`sending` 只是浏览器本地状态，不触发成功脉冲。EOS 响应 ID 匹配后改为精确所有者状态。`InProgress` 不是终态；`Completed`、`Failed`、`Cancelled`、`Rejected`、`OutcomeUnknown` 是显示终态。`Failed` 显示明确失败标签和原因。`OutcomeUnknown` 显示明确不确定标签，绝不触发运动命令重试。发送超时或桥接断开且没有关联响应时显示**提交结果不可用**，保留生成的 ID 供排查；状态／修订／运行恢复后只能进行受保护的读取，不能重发命令。后续读取遇到不透明 ID 或离线时显示**结果不可用**，保留上次已知的 `InProgress`，不改成终态。

结果读取每条命令最多一个进行中请求，一秒后开始；连续读取错误退避，上限十秒；自动轮询满六十秒或十二次（先到者）即停止。文档隐藏时暂停，恢复时先复查 IsOS 运行与修订；手动刷新也执行相同检查。UI 显示已暂停的 `InProgress` 记录和刷新操作，不能暗示完成。切换页面和 Stop 会中止读取。这些限制只适用于只读结果查询，绝不用于再次提交运动。

部署 **Stop** 即使 EOS 离线、图只读、活动修订不符、结果未知或客户端暂停，也要通过 IsOS `/api/isos/v1/process-control` 保持可见可用。点击 Stop 只是待处理意图，直到 IsOS 报告其受治理的终态或未知结果；UI 不能把命令 `Cancelled` 等同物理停止。R07 在最小命令卡旁提供 IsOS Stop 操作；U03/U04 后续在浮动面板中保持其可见。此 R07 部分包含最小类型化发送方／消费者，但不包含 U04 的完整原语控件或图信息卡。

Stop 响应包含 `stopped`（已回收的直属子进程 ID）和独立的 `terminal` 对象或 null；它不是 `process-status` 快照。点击 Stop 时立即隔离未完成的 EOS 结果读取，并阻止在该运行中再次调用。收到 IsOS 响应前显示等待。只有格式正确的 `terminal.outcome:"confirmed"`、`terminal.run_id` 与点击时捕获的已知运行相同、原因为 null 且子进程结果均已确认时，才显示**停止已确认**。格式正确的 `unknown` 以告警显示固定 `reason_code`。`terminal` 为 null、缺失、格式错误或运行不符时显示**停止未获确认**；若权威状态为 `run_id_state:none` 且 `terminal:null`，则显示没有需要停止的受管运行。传输失败显示**停止响应不可用**，不会自动重试。收到响应后，另行读取新的 `GET /process-status` 更新实时运行与修订；刷新失败不能抹去已知 Stop 结果。Home、Graph 和 EOS 的 Stop 控件采用相同解释。

重复的子进程 ID 不能构成格式正确的终态。即使从未运行，`terminal:null` 且 `stopped` 非空也属于未确认。明确的 IsOS API 错误显示为**IsOS 拒绝停止**并附带所有者消息，与传输不可用分开。

`stopped` 中的每个 ID 都必须出现在 `terminal.processes` 中；确认终态若缺少对应子进程记录，仍显示未确认。
