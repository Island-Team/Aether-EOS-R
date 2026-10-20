# Diagnostic REPL

`src/eos/ext/repl` 是 `eos-north-http` 的独立 HTTP/SSE 客户端；它不包含 gRPC stub，也不存在直连 EOS 的路径。

```sh
./bin/eosd --config "$PWD/etc/eos/eos.example.toml"
./bin/eos-north-http
./bin/eos-repl
```

开发环境也可使用 `./eos-launch.sh` 监管 simulator → EOS → north HTTP → REPL； 生产环境仍分别管理各进程。

## 选项

| Option | 默认值 | 含义 |
| --- | --- | --- |
| `--host ADDR` | `[north_http].host` | north HTTP host |
| `--port N` | `[north_http].port` (`16005`) | HTTP/SSE port |
| `--config PATH` | `etc/eos/north-http.toml` | north HTTP 默认值来源 |
| `--contracts PATH` | `EOS_CONTRACTS`，然后 `ISLAND_SHARE` | 离线或旧版 server 的描述与 advisory check/hint fallback schema |
| `--connect-timeout-ms N` | `2000` | north HTTP liveness connection deadline |
| `--timeout-ms N` | `630000` | unary request deadline；SSE reconnect 使用内部有界 backoff |
| `--no-history` | off | 禁用 history |

未知 option、缺失值、错误数字和非正 timeout 在启动前被拒绝。north HTTP live 后 客户端调用 `Discover`；若 EOS 未 ready，则显示 north HTTP 的 `503`。成功时在 prompt 中显示 robot name。

## 命令

```text
discover; status; audit-status; resources; read <uri>; watch <uri>
capture <robot> <camera> [1|2]
observe-objects --json [{...}]; perception-roles
call <primitive> <ver> <device|-> ...; cancel-motion <command-id>
auto base [device-id]  # 键盘控制流式 Base 导航；默认 base-0

skills; skill <name> <version>; start <name> <version> ...
executions; execution <id>; watch-execution <id> [sequence]
kill <id>; pkill <skill> [version]; signal <id> <name> ...
journalctl <id> [journal-sequence]
top [skill [version]] [--status STATUS[,STATUS...]]  # 单次快照
htop [skill [version]] [--status STATUS[,STATUS...]] # 实时监控

workflows; workflow apply [--json|--prompt]; workflow <id>; workflow watch [id]; workflow templates;
workflow show <template>; workflow run <template> [id]; runtime
describe primitive <name> <ver> [device]
describe skill <name> <version>
refresh; set full on|off; connect [host] [port]; disconnect; help; quit
```

`observe-objects` 使用 ProtoJSON 请求对象，必填 `robotName`、`deviceId` 和 `modelId`，可选 `sourceTimestampNs`。`perception-roles` 列出已配置的感知模型角色。

primitive、skill 与 signal command 可用 `key=value` 快速输入，会转换明显的 null、boolean、integer、number、 array 与 object；需要保留为 string 的数字应加引号，错误 array/object 会在本地 拒绝。`--json {…}` 接受完整 object；不带值的 `--json` 打开 multiline editor： Enter 提交，Ctrl-J 换行，Ctrl-C 取消，错误 JSON 可继续修改。

`auto base [device-id]` 需要交互终端。每个方向按键事件发送一个流式导航目标：W/S 为前后方向，A/D 为左右方向（左为正 Y），J/K 为偏航；Q 退出。`[` 降低、`]` 提高目标步长，每次调整 25%，范围 25% 至 200%，默认 100%。默认平移步长为 0.24 m，偏航步长为 0.45 rad；这是目标缩放，不是标定后的速度单位。每次请求使用新的 command ID。REPL 不合成按住状态的更新；长按依赖终端的按键重复事件。REPL 不合成按住状态的更新；长按依赖终端的按键重复事件。所选后端定义目标替换、空闲与取消行为。退出、中断、终端断开或请求失败时，REPL 不请求 `base.posture stand`；`motion.stop` 仍可用。
```text
call arm.movej 1.0.0 arm-0 reference=absolute joint_positions_rad=[0,0,0,0,0,0]
start observe_human 1.0.0 --timeout 30m robot_name=robot-0 gimbal_device_id=gimbal-0
signal EXECUTION_ID application.signal --json {"field":"value"}
workflow apply --json {"definition":{"workflowId":"demo-1","actions":[{"actionId":"stop","primitive":{"name":"robot.stop","version":"1.0.0","deviceId":""},"parameters":{}}]}}
workflow templates
workflow show register_host
workflow run register_host debug-register-host
workflow watch debug-register-host
```

## 已注册工作流模板

EOS 启动时会注册 `<share-root>/workflows` 中校验通过的 JSON 模板，并通过 `WorkflowService` 发布目录。REPL 连接后像发现 skill 一样从 EOS 获取模板。 模板不是已经运行的 ActionList：`workflow templates` 列表，`workflow show <name>` 查看定义，`workflow run <name> [workflow-id]` 请求 EOS 复制并提交模板。不显式提供 ID 时，EOS 会在模板的 `workflowId` 后加 UUID，避免多次调试时实例 ID 冲突。

`workflow watch` 观察全局 ActionList event stream，按 `q` 只退出观察。 `workflow watch <id>` 只显示指定 workflow；`q` 仍只退出观察，Ctrl-C 会提交一个 no-op replacement 来 supersede 目标 workflow。取消是 cooperative 的，因此请求被接受 只表示已经请求终止；旧 workflow 可能短暂处于 `SUPERSEDING`，随后才进入 `SUPERSEDED`。

上面的 `application.signal` 及 payload 只是语法示例；实际调用必须使用 `describe skill` 的 `acceptedSignals` 所声明的名称与 schema。当前随仓库提供的 示例 skill 不接受应用 SIGNAL；manager 保留的终止操作使用 `kill`。

primitive、skill start、signal 与 workflow 支持 `--prompt`。guided entry 先显示 required field，再让用户选择 optional field，提供 type/default/description、 enum、boolean 与数值范围，在本地检查 required field、基本类型、choice 与直接数值边界，显示最终 JSON，并在提交前确认。 workflow submission 使用 `--json` 或 `--prompt`；其他参数化 command 仍保留 `key=value`。primitive 的 `describe`、prompt、validation hint 与 completion 优先使用 discovery 返回的 per-device `parameters`；仅在旧版 server 缺失该字段或 离线时使用本地 `--contracts`。`describe primitive` 还显示独立的 discovery limits； `describe skill` 使用 live descriptor。复杂 root conditional 与嵌套 array/string constraint 仍以 EOS 校验为准，REPL 不复制一套不完整 validator。显式空 remote schema 表示 primitive 接受 `{}`，不会触发本地 fallback 或 raw entry。脚本可使用完整的非交互 `--json`。

交互终端支持 UTF-8 编辑、方向键、去重 history、Ctrl-R、highlight、bracketed paste、hint 与 context-aware Tab completion。piped input 无 prompt。history 最多 1,000 条，以用户权限保存到 `$XDG_STATE_HOME/island/eos-repl/history`，或 `$HOME/.local/state/island/eos-repl/history`；敏感命令请使用 `--no-history`。

watch、journal 与 execution monitor 使用 modal UI，按 `q` 只关闭当前视图。stream 保存最后处理的 SSE `id`， 并用 `Last-Event-ID` 重连；显式 sequence 始终优先。关闭 watch 会断开 HTTP， north HTTP 随即取消 upstream gRPC context。每个 session 最多接纳 16 个并发 watch，并将 单个缓冲 SSE event 限制为 8 MiB。错误保留数字 gRPC code、message 与 request ID。HTTP rejection 或 malformed SSE error 会结束当前 modal watch，不会继续 attach 在已终止的 stream 上。

`start` 在成功启动后复用 execution SSE watch 自动 attach，并在 terminal status 时返回；`--detach` 使其在 launch 成功后立即返回。未提供 `--timeout DURATION` 时不发送 execution timeout。attached mode 中，Ctrl-C 提交 cancellation 后立即 detach，不终止 REPL；`p` 暂停，`r` 恢复，`s` 打开 signal/payload prompt，`d` 或 `q` detach，`?` 显示控制键。terminal hangup 仅 detach，且所有 exit path 都恢复 terminal mode。

大体积 base64 默认隐藏，可用 `set full on` 显示。日志独立写入 `var/log/eos/latest/repl/`，与 north HTTP/EOS 使用同一 request ID，但不记录 payload。

skill execution view 会显示 snapshot 携带的稳定 terminal reason 与有界 operator message。脚本必须按 status/reason 分支，不得解析 message。失败可与 EOS 唯一的 `skill_execution_terminal` warning 关联；message 已清理，legacy skill 或 cancellation 可能没有 message。

完整 wire 规则见 [north HTTP/WebUI 契约](../../interfaces/external/http_cn.md)、 [northbound interface](../../interfaces/grpc_cn.md) 与 [primitive reference](../../interfaces/actions-payloads_cn.md)。
