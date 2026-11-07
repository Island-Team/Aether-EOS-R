# Terminal operator console（中文）

> 本文是 [Terminal operator console](tui.md) 的中文伴随文档。代码、命令、路径、接口名称和字段名保持与英文原文一致，以便直接用于开发与运维。

`libexec/egia_ext` 提供源码检出环境的终端操作员界面。它以子进程启动 EGIA，消费北向 WebSocket 流，并渲染状态、任务、上下文、提示词、模型活动、primitive、原生 skill 和日志。它是客户端，不是另一条运行时路径。

## Data sources

- `/health` and `/backend` drive lifecycle status;
- `/missions/stream` drives the mission tree;
- `/missions/command` accepts short-lived mission create, update, priority, and cancel requests;
- `/context`, `/prompts`, and `/model_io` expose model-facing state;
- `/primitives` exposes primitive targets; `/skills`, `/skills/command`, and `/skills/execution` provide the executable native-skill catalog, commands, and ordered execution watches;
- perception and part routes expose current attributed observations.

不可用的数据源显示为 unavailable。控制台不会伪造硬件、skill、感知或结果状态。

控制台属于受监管 launch 时，log pane 跟随当前 Island session replay。该有序流包含 Island lifecycle event 以及本地 EOS、EGIA 和 TUI event；原始 timestamp 保留显示，`collector_sequence` 定义采集顺序。文件 rotation 不会清空 pane；attach mode 会在受保护的 active-session metadata 被替换时切换到新 session。Malformed record 会被忽略。EGIA 在 Island 外独立运行时，pane 回退到 `var/lib/egia/egia.jsonl`，并继续以 launch boundary 隐藏旧运行记录。已经通过 Island collector 收到的 structured child stderr 不会重复显示；非结构化 traceback 仍然可见。

## Navigation

当前聚焦的 pane 会填满控制台，其余 pane 仍以带标签的分隔线显示。可直接使用 `Alt+1` context、`Alt+2` missions、`Alt+3` primitives、`Alt+4` skills、`Alt+5` model stream、`Alt+6` logs 或 `Alt+7` status 选择 pane。skills pane 显示 parameter schema、result type 和本控制台观察到的每个 execution。终端将 Alt 编码为 Escape 加数字，以及发送 8-bit Meta 字符的两种方式均受支持。PageUp/PageDown 按页滚动当前 pane；若终端拦截或错误报告 PageUp/PageDown，可使用等价的 `F7`/`F8`。

prompt 为空时，Up/Down 逐行滚动，Home/End 跳转到最旧/最新内容，Left/Right 浏览 primitive 页面或较宽 context pane 的 primitive-guidance 列。prompt 中已有文本后，arrows、Home/End 和 Left/Right 改为编辑该命令。`help` 显示完整映射。

## Command line

`> ` prompt 是 shell 风格的会话命令行。终端支持颜色时，已知命令名为 cyan，未知命令名为 red；inline hints 和 completion candidates 为 gray。Tab 可补全命令名、mission 和 skill subcommand、可见 mission ID prefix、已跟踪的 execution ID、priority band、pane 名、backend alias、显式 primitive target，以及精确 skill name 和 version。primitive 和 skill argument completion 从实时 JSON Schema 读取字段，将 required field 排在前面，并补全 constant、enum 和 boolean。多个候选保留为 hint，直到更多输入能够消除歧义。

Enter 执行命令。输入文本后，Up/Down 浏览 history；prompt 为空时，Ctrl-P 和 Ctrl-N 也可浏览 history。Ctrl-R 反向搜索 session history。支持标准编辑键：Left/Right、Home/End、Ctrl-A/E、Ctrl-B/F、Backspace/Delete、Ctrl-U（清除整行）和 Ctrl-W（删除一个词）。命令 history 仅在本次 session 中保留，绝不写入磁盘，因为 operator call 可能包含敏感参数。

## Commands

`chat "sentence"` 发送听觉输入。`call <primitive>@<robot>/<device> key=value...` 向 `/primitive/invoke` 发送显式定位的请求；原有 `call <target> {"key":"value"}` JSON object 形式仍可用于嵌套或类型敏感的 payload。可以用 shell quote 保留空格，例如 `text="hello world"`；number、boolean、array 和 object 等 JSON 形式的 value 会保留其 JSON 类型。缺少目标时显示输入错误，而不会猜测目标。输入或补全 target 后再次按 Tab，可插入其必填 parameter；不带 argument 的 `call <target>` 仍会打开完整 field guide。规划器动作和操作员调用共用同一条安全路径。

`skill start <name>@<version> robot=<alias> timeout=<seconds> key=value...` 启动精确的 native skill。控制台生成新的 UUID request ID，要求显式提供已配置的 robot alias，并仅接受大于零且不超过 3600 秒的 timeout。argument 使用与 `call` 相同的 shell-safe typed `key=value` 形式；Tab 根据所选 skill 的 parameter schema 补全字段。

`skill show <execution-id> robot=<alias>` 读取完整 snapshot，`skill watch <execution-id> robot=<alias>` 跟随有序 snapshot，`skill cancel <execution-id> robot=<alias>` 请求 cooperative cancellation。skills pane 显示 request 和 execution ID、status、sequence、cancellation state、reason 或 message，以及 terminal result。传输中断后，watch 使用最后显示的 sequence 重连，并在 terminal status 时结束。命令错误写入 log pane，不会虚构 execution state。

`mission` 管理当前 mission table：

- `mission add "order"` 创建 operator mission；
- `mission list` 聚焦 mission pane；
- `mission show <unique-id-prefix>` 打开当前 mission 详情，其中包括完整 ID、priority band、state 和 document revision；
- `mission edit <unique-id-prefix> "order"` 替换 pending mission 的 order；
- `mission priority <unique-id-prefix> low|normal|high|urgent` 修改 priority band；
- `mission cancel <unique-id-prefix>` 取消 pending mission。

只有 mission subcommand 使用 shell 风格 tokenization，因此带引号的 order 可以保留空格，同时不会改变其他顶层命令既有的解析行为。命令接受完整 mission ID 或本地唯一 prefix。控制台只根据当前 `/missions/stream` snapshot 解析 prefix，在本地拒绝歧义或不存在的 prefix，并在 compare-and-swap mutation 中发送完整 ID 和当前 `document.revision`。每次 mutation 使用一个短生命周期的 `/missions/command` WebSocket 连接，记录带 operation 类型的 acknowledgement 或 rejection，并等待 `/missions/stream` 提供权威刷新状态。Cancelled mission 会保留显示，并使用明确的 cancelled state。

`repl [backend-alias]` 会暂时离开全屏控制台，并在同一终端上运行外部 EOS 诊断客户端，仅用于当前 EOS resource 和 primitive 诊断。skill 操作属于上述北向 `skill` 命令，不假定 EOS REPL 提供 skill execution。选中的 backend 必须被 `/backend` 报告为 `up`；配置多个 backend 时必须提供别名。endpoint 来自该 backend 的实时投影，`eos-repl` wrapper 则从 `island.toml` 选定的 EOS launcher 旁解析。backend、endpoint 或 wrapper 不可用时，控制台保持运行并报告错误。

EOS REPL 占用终端期间，EGIA 进程和所有控制台 feed 继续运行。在 EOS REPL 中输入 `quit` 或 `exit`，或按 Ctrl-D，即可返回原有 TUI 状态。REPL 以非零状态退出时，终端恢复并完整重绘后，TUI 日志会报告该状态。

## Shutdown

从 TUI 交互退出时，控制台会请求子进程停止并进行有界等待。TUI 由组合式 `bin/island` 启动时，其正常退出还会通知产品 launcher 停止并回收该 launcher 拥有的 EOS 进程。退出 EOS REPL 只会返回 TUI。已安装的服务应由其所属的服务管理器管理；控制台不直接拥有 EOS 或模型服务器进程。
