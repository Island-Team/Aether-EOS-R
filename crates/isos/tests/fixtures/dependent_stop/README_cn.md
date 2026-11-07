# R06 IsOS 依赖停止用例

[English](README.md) · [归属方契约](../../../../../docs_cn/isos/interfaces/cores/dependent-stop_cn.md) · [设计](../../../../../docs_cn/isos/design/server_cn.md)

**仅设计用例。** `cases.json` 冻结 57 个桩和真实归属方实现测试的预期结果。`python3 tests/fixtures/dependent_stop/check_cases.py` 检查形状与安全关键预期；它不运行进程，也不能证明实体停止。实现必须按已评审的 EOS R06 契约把终结收据绑定到选中子进程、运行 ID 和观测 PID。用例中的 `eos_receipt` 标记是测试场景，不是 EOS wire 字段。`eos_receipt_confirmed.json` 和 `eos_receipt_unknown.json` 冻结经评审的 EOS schema-1 `eos_terminal_stop` stdout 帧；`receipt_mutation` 从确认帧派生解析器反例。

使用一个干净的 `ISOS_HOME` 重放正反用例：直接 CLI、HTTP 和归属方 socket Stop、附着 Ctrl+C、Restart／回滚、提供者崩溃、管理器 Shutdown、事件轮转和故障。对比前后的进程组成员、真实 EOS/MuJoCo 子进程及 socket，并比较运行 ID 与授权，但不打印授权值。完整输出保存在忽略的 `logs/test/<run-id>/`。停止未知时必须阻止新候选，不能因为 PID 消失就写 `deployment_stopped`。最终真实归属方重放需要 EOS 和固定 MuJoCo `0.1.1` 软件包的两个场景；桩可测试顺序，但不能关闭 R06 门槛。

评审用例要求组长在 Stop 前退出但保持未回收时能安全地向原组发信号，并要求确定性的组长提前回收／PGID 复用竞争，证明**没有**向复用组发信号；最迟在 `deadline - 2,000 ms` 开始 KILL，并有界回收及发布；信号前持久化未知意图、从该意图恢复崩溃，以及 P07 发布后、子进程启动前原子重置新运行。意图重命名或父目录 fsync 卡住后，即使在调用方收到未知之后完成，磁盘上也只能有未知。实时确认在管理器重启后降级为未知；历史 `deployment_stopped` 不能作为安全依据。恰好等于期限时，实时栅栏发布未知。过晚线程不得发布确认。

收据用例要求 IsOS 在 P06 归一化之前只读取选中 EOS 的原始 stdout 管道，绑定运行／PID／修订／后端／所选部件，拒绝重复或未知键、不完整／超长／第二条／过晚行，并忽略其他进程管道，并要求 EOS 零码退出及两个进程组排空。`managed-stop-v1` 仅是预检支持声明。EOS 停止界限为 15 秒；IsOS 缺省在 30 秒总期限内给 EOS 20 秒宽限。用例检查器校验形状与预期，实现测试仍须注入真实原始字节与时序。

原因优先级已冻结：选中 EOS 管道上的错误运行 ID 为 `eos_receipt_invalid`；只有其他管道有收据时，选中收据仍为 `eos_receipt_missing`。终结结果未决定时，达到或超过总期限先于 EOS 宽限或解析原因选择 `stop_deadline_exceeded`。严格早于总期限的过晚收据为 `eos_receipt_late`；已发布结果不再改变。
