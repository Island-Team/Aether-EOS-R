# EGIA 本机安全重置与操作恢复

[English](../../../docs/ei/interfaces/safety-reset.md) · [事件](events.md) · [首次使用](../develop/build-and-run.md)

EGIA 拥有紧急锁存和持久操作检查点。IsOS 管理器、HTTP、WebSocket、图边和模型都不能重置锁存。外部处理 EOS estop 并停止 EGIA 后，同一主机和 UID 的操作员可在交互终端运行 `./bin/egia safety reset --state-root <island-state-root> --episode <episode-id> --graph-revision <selected-sha256>`，输入精确短语 `RESET EGIA SAFETY`。命令取得运行时排他 `safety.lock`，只写入一次性授权；它不会直接解锁。对于 IsOS 托管部署，`<island-state-root>` 应填入 `<isos-data-root>/state/<deployment>`；使用启动 EGIA 时传入的精确包含根目录，不要填其 `egia/` 子目录。

## 仅所有者可访问的状态

文件位于 `<island-state-root>/egia/safety/`；操作检查点位于 `<island-state-root>/egia/operations/`。IsOS 在所选 `0.1.0` 托管启动时以 `--state-root` 提供部署专属的包含根目录 `<isos-data-root>/state/<deployment>`；同一部署的 Run/Stop/Restart 保留该目录。`<isos-data-root>` 在设置 `ISOS_HOME` 时为 `<ISOS_HOME>/data`，否则为 `${XDG_DATA_HOME:-~/.local/share}/isos`。`egia/` 下的字节归 EGIA 所有，不是图字段或不可变生成配置。目录权限为 `0700`，普通文件为 `0600`，写入使用同目录临时文件、文件 fsync、原子替换和目录 fsync。符号链接、错误所有者或权限、错误 JSON、超长记录、未知 schema 均关闭准入。EGIA 独立启动继续使用现有布局或显式 `--state-root` 的语义。

| 字段 | 类型／单位 | 用途及验证 |
| --- | --- | --- |
| `schema` | 精确字符串／无 | `egia-safety-history-v1`、`egia-safety-latch-v1`、`egia-safety-reset-v1`、`egia-operations-history-v1`、`egia-operation-index-v1` 或 `egia-operation-v1`；未知版本失败。 |
| `identities` | alias → EOS robot ID 对象／无 | 非空受影响别名集合；键为 1–64 个 `[A-Za-z0-9_-]`，robot ID 字符串最多 128 字节；空 ID 表示未知，不能通过重置。 |
| `episode_id` | 32 位小写十六进制／锁存期 | 每次新的锁存产生新值；持久 marker、锁存和一次性授权必须精确一致。 |
| `graph_revision` | 64 位小写 SHA-256／所选图 | 锁存与授权绑定当前 IsOS 所选图；独立锁存测试使用全零哨兵值。操作检查点保存所选修订或 `standalone` 字面量。 |
| `nonce` | 32 位小写十六进制／无 | 一次性重置请求身份；下次启动消费，不重用。 |
| `reserved_count`、`sequence` | 正数 64 位整数／预留顺序 | 单调历史计数和唯一、从 1 开始的索引序号；索引集合须完整连续。 |
| `operation_id` | 字符串／EOS 幂等键 | 1–64 个 `[A-Za-z0-9_-]`；在 gate 外发前生成或验证，在 Submit 前持久保存。 |
| `alias`、`primitive`、`device_id`、`mission_id` | 字符串／归属身份 | 精确恢复目标；`mission_id` 可为 null。原语和设备字符串最多 128 字节，别名和任务 ID 使用不透明 ID 语法。 |
| `eos_robot_id` | 非空字符串／EOS 身份 | 操作预留时记录；恢复查询前须与完整 EOS 发现一致。 |
| `state`、`outcome` | 封闭字符串／无 | 操作为 `pending` 或 `terminal`；终结结果为 `succeeded`、`failed`、`cancelled`、`rejected`，且须有匹配 EOS 收据。 |

重启时 EGIA 载入所有待决操作，仅用保存的 ID 调用 EOS `GetOperation`。缺失、错误、非终态或不匹配的收据保持未解决；不能为同一旧物理动作重新 Submit。损坏的检查点阻止正常启动，不会静默丢弃不确定性。

所有者进程重启测试在两个 Python 解释器之间保持同一个 stub EOS 操作表、grant、所选修订和模拟 run ID。第一个进程在 Submit 前预留操作，stub 保存终态收据后、EGIA 结算前退出；第二个进程运行正式的 EGIA 待决检查点启动路径。完整发现先于用保存 ID 调用 `GetOperation`；检查点结算且没有第二次 Submit。缺失收据的案例保留待决 ID 和锁存，并拒绝后续重置。该测试覆盖 EGIA 所有者进程边界；当前 IsOS R06 在所选 EGIA 子进程故障后停止整个部署，因此不能据此声称受监管的同一 run 子进程替换已实现。

仅测试使用的私有 `A03_EVIDENCE_DIR` 钩子在各工作进程退出后复制原始所有者状态，标签为 `positive-before`、`positive-after`、`negative-before`、`negative-after` 和 `negative-refused`。每个新快照目录权限为 `0700`；复制的文件以及记录哈希、字节数和权限的确定性清单均为 `0600`。精确文件集为 `operations-history.json`、所选操作的 `.index` 和 `.json`；仅后两个 negative 快照增加 `safety-history.json` 和 `safety-latch.json`。`negative-after` 在写入重置授权前复制。缺失、符号链接、错误类型、错误所有者或错误权限的源文件会导致失败；钩子绝不复制 `safety-reset.json`、grant 字节或其他所有者状态。未设置环境变量时不创建证据目录，也不影响运行时。

重置请求仅消费一次。EGIA 要求精确的所选 alias 覆盖、每个 alias 的持久 EOS robot ID、没有未解决操作，以及两次新鲜完整且匹配所选图 revision 的 EOS Discover/GetStatus/ListCapabilities 代际。两次检查均要求 `Discover.robot.estop=false`、ready health 和安全的实时状态。任何检查失败都保留锁存。新的 EOS 或 EGIA 进程身份本身不能授权解锁。

持久 `safety-history.json` 标记曾经锁存及当前 episode；曾锁存而锁存文件缺失时启动失败。`operations-history.json` 保留单调递增的 `reserved_count`，它先于各 ID 的 `.index`（含唯一且从 1 开始的 `sequence`）和数据文件写入；终态索引仍保留。即使其他操作仍在，缺失任一检查点或索引时启动也失败。终结检查点不可改写。一次性授权在运行锁取得后、配置／发现／任务检查点读取前消费，即使本次启动早期失败也不会留到下一次使用。托管启动即使 `mission_preload=false` 也会恢复有效任务检查点或关闭准入；不会清除先前未完成任务。

运行本机重置命令前，从仅所有者可读的 `safety-history.json` 取得 `episode_id`，并从 IsOS 已应用图取得当前所选修订。修订改变或产生新锁存期后必须重新取得操作员授权，不能重用旧请求。
