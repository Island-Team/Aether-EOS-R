# P06 统一 JSONL 事件夹具

[English](README.md) · [归属契约](../../../../../docs_cn/isos/interfaces/cores/events_cn.md) · [设计](../../../../../docs_cn/isos/design/server_cn.md)

`cases.json` 冻结 37 个成功、拒绝、抑制、生命周期及已审核归属目录场景，案例名称供实现测试稳定引用。检查器验证案例目录及代表性原始子进程／存储记录边界，包括 8,064/8,065 与 8,192/8,193 字节。执行 `python3 tests/fixtures/jsonl_events/check_cases.py`。Rust 单元与真实进程测试覆盖运行时收集器。

实现时对 `LogStore`、`Supervisor`、管理器状态和所选 stub 子进程逐案测试。原始超长或规范化后超长记录须增加有界 `log_ingest_rejected` 原因计数，不得复制原行；丢弃原始超长行后仍须读取下一条有效行。把所选 `eos`、`bridge`、`config`、`egia` 来源绑定至直接子进程角色，比对活动 `run_id`，拒绝原始 `pid`、节点成员指针、EOS 对 EGIA 节点的声称、未知子进程事件码及子进程对 IsOS 抑制码的声称。schema-1 `log_append` 冒充案例对语法有效且非保留来源的请求保留 `accepted:true`，同时检查存储的来源／事件码／消息被固定为 `legacy_external` 管理器值；保留来源仍被拒绝。EOS runtime 专属的 `eos_events_suppressed` 固定在 EOS 角色策略内。EGIA 事件准入使用已审核的 16 码 `egia-role-policy.json` 归属目录。测试管理器会话下的启动前失败、精确软件包 pin 与已验证收据来源、候选／恢复运行 ID、抑制、轮转和不阻塞停止。最终归属边界测试回放真实 EOS 与 EGIA 的 stderr。

A03 固定归属方精确的 `egia-jsonl-role-policy-a03-v2` 24 码夹具，SHA-256 为 `fae3cfc900956d26a66e3304943b492799f483ca91678de450257e0ce19c5491`。原有 16 项不变。Rust 测试覆盖封闭事件码／字段／级别准入、不同安全拒绝的关联与原因、首次写入失败后重试，以及终结摘要顺序。
