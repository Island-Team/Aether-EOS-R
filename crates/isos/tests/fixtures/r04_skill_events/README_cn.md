# R04 所选 EOS 技能事件——设计夹具

[English](README.md) · [IsOS 事件归属契约](../../../../../docs_cn/isos/interfaces/cores/events_cn.md) · [游标归属契约](../../../../../docs_cn/isos/develop/owner/log-cursors_cn.md) · [EOS 归属契约](../../../../../docs/eos/interfaces/events.md)

`cases.json` 固定 EOS SkillService 精确提交 `192fbfa6fbed1e9b6a20dd8dc95501625b02d081`，包含 31 个所选子进程接纳／拒绝案例和十个 P07 游标场景。`python3 tests/fixtures/r04_skill_events/check_cases.py` 检查夹具结构与参考归属策略；它**不**运行 IsOS。实现测试覆盖所选 EOS runtime 分支，并保持旧 P06 案例通过。

正例包括 queued、running、completed、精确选择的 `0..1` 空点段版本、物理结果不确定的 failed、worker 未回收的 failed 及固定停止故障。空段案例记录当前 P01／EOS 生产者兼容性；独立的预停止语法门禁仍待完成。所选技能版本含 build metadata；MuJoCo 专属包字段规则或只允许小写版本的规则都不适用。负例拒绝 bridge／EGIA 冒充、错误运行、节点声明、消息／级别／阶段／原因不匹配、任意 `worker.*` 后缀、内部错误码、缺失或多余字段、错误类型、无效包版本和执行 ID、原始 PID 冒充、停止字段变化、共享抑制码及未知 guest 事件码。权威来自所选 EOS 角色，而不是图或子进程 capability 声称。

游标场景须把每条已接纳原始事件经真实所选 EOS 子进程管道送入，并确认精确脱敏记录出现在 `eos.log` 与 `events.log`，PID 由 IsOS 观察。被拒绝的事件不得进入任一文件。运行中的首条合格事件设为技能失败；queued／running 保持锚点待定。分别无筛选和使用排除该事件的来源／关联条件查询 `view=current`，锚点都仍是失败。之后接纳的同码事件须按另一位置显示在锚点后。用旧游标跨保留备份（`rotated:true`、无丢失），再使段过期（`log_cursor_expired`、封印锚点保留）。注入收集／写入缺口，报告真实计数或 null；Stop 后保留最后运行。实时就绪由 status 判断。完整回放与轮转输出保存在忽略的 `logs/test/<run-id>/`。

IsOS 候选预检还须在停止活动部署前执行所选技能 P01 版本限制（包 ID 最多 96 字节、版本最多 64 字节及归属语法）。该预检是另一个待完成实现门禁。EOS worker 回收与 R06 物理停止顺序各需独立证据；日志行不能证明执行器已停止。
