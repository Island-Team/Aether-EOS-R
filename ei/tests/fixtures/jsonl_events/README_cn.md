# EGIA P06 事件夹具

[English](README.md) · [归属接口](../../../../docs_cn/ei/interfaces/events.md) · [设计](../../../../docs_cn/ei/design/events.md)

`egia-role-policy.json` 是 16 个 EGIA 事件码、固定消息、级别（含按结果及停止状态变化的级别）、节点规则及类型化字段／枚举域的规范机器清单。IsOS 必须将此审核目录固定到所选子进程角色策略后才接纳 EGIA stderr。`cases.json` 命名 45 个成功、拒绝、抑制和故障案例，包括 `egia_stopped` 四种状态／级别组合，供实现测试使用。`python3 tests/fixtures/jsonl_events/check_cases.py` 验证目录及代表性 ID、角色、字段和字节边界。检查器是设计证据，不是运行时发射器测试。

实现时用假时钟、stub EOS 收据、隔离私有 replay 和捕获的托管 stderr 逐案回放。然后在 IsOS 下选择真实 EGIA 子进程，比对 `egia.log` 和 `events.log` 的同一接纳事件，验证首个故障和终结事件，并证明收集器故障时归属 replay 仍持续。EGIA 不能安全发出事件之前的启动故障由 IsOS 监督记录。完整输出保存在忽略的 `logs/test/<run-id>/`，命令和结果汇总在设计页。
