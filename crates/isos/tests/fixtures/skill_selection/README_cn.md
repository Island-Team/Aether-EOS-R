# R04 IsOS 技能选择用例

[English](README.md) · [所有者契约](../../../../../docs_cn/isos/interfaces/packages/process_cn.md) · [设计](../../../../../docs_cn/isos/design/packages_cn.md)

`cases.json` 固定 IsOS 设计边界，不表示实现已经通过。基线是保存的 `0.1.0` 图、一个 role 为 EOS 的 `@v0.1` component、明确的 schema-2 V2 generation、已验证且 `kind:"skill"` 的 `example/native-skill@0.1.0` P01 发布、一个普通文件 `lib/island/skills/example.skill.so`，以及声明 `host-skill-abi-v2` 的 EOS stub。`selection.example.json` 展示生成结构，其中摘要和路径是合成占位符；测试以真实摘要和私有路径替代。解析固定用例时不运行 guest 代码。

实现应把每个 ID 对应到单元或跨所有者 stub 集成测试，把命令、IsOS/EOS 精确提交和完整输出保存于忽略的 `logs/test/<run-id>/`。成功必须证明生成文件被 manifest 列出、P04 generation 可重新计算、P01 重新验证、环境变量只到 EOS，并且 EOS 验证和暂存选中的字节之后才报告目录就绪。每个停止前失败均保留旧 PID、active revision、run ID 与 R03 grant。随后 EOS worker 拒绝属于候选运行，不能误报为 IsOS 停止前失败。P01/P04/P05 代码优先。EOS 测试另行覆盖 ABI 大小协商、无效描述符、能力拒绝和 worker 回收。

`catalog-setting.example.json` 固定 IsOS 拥有的 `@v0.1` setting metadata，依赖 EOS 的通用 object-array 支持。运行 `python3 tests/fixtures/skill_selection/check_cases.py` 校验用例 ID、两个示例结构和中英文链接。

## 候选目录预检用例

[`preflight_cases.json`](preflight_cases.json) 固定 18 个 IsOS 所有者用例，对齐已独立批准的 [EOS 预检设计](../../../../../docs_cn/eos/interfaces/skills/native/host-abi_cn.md)。R04-IPF07 用已链接且 P01 有效的发布检查更早的 IsOS 选中 pin 图门槛；它不调用 EOS 探测。成功和拒绝对象是合成的封闭单行协议示例；用例是冻结的单元、stub 或跨仓重放要求；仅检查用例结构不能证明实现已通过。精确合并版本的 EOS/MuJoCo 限定重放已通过旧 Stop 确认与未知、重试阻断、保留日志轮转和精确 grant token 扫描；范围和限制见[验收证据](../../../../../docs_cn/isos/design/packages_cn.md)。每次故障注入都从仍在服务的旧运行开始，记录 PID 集合、run ID、活动修订、R03 grant 和 run scope。只有确认候选 grant 撤销与探测进程回收，才能认定干净拒绝。候选 run scope 尚未发布，首条预检故障须按 manager-session correlation ID 查询。全局硬期限为 90 秒。探测成功后仍须停止前重检字节/grant，并由 EOS 在实际启动时独立准入。R05/R06 实体停止次序是独立门槛。

运行 `python3 tests/fixtures/skill_selection/preflight_cases.py` 检查用例 ID 与严格的结果示例；脚本不会运行 EOS，也不证明事务已实现。后续集成命令、完整输出、精确 IsOS/EOS 提交及实际子进程 PID 存放在忽略的 `logs/test/<run-id>/`。
## 成功探测后旧运行停止与候选授权撤销用例

[`retirement_cases.json`](retirement_cases.json) 固定私有探测 `ok:true` 且最后停止前复核通过后的六个 IsOS 用例，并与独立 R06 可检查停止契约组合；R06 [#57](https://github.com/Island-Team/isos/pull/57) 的 unknown Restart 候选撤销缺口已由 R04 [#58](https://github.com/Island-Team/isos/pull/58) 实现。旧 terminal 确认后只允许发布一次候选范围。旧 terminal 为 unknown 时，禁止候选及恢复启动，保存旧 run 身份和图字节，status 为 degraded，并发布旧运行范围的 `deployment_stop_unconfirmed`；同时检查未发布候选 grant 撤销。Stop 已开始，旧 PID 不保证不变。候选撤销不确定时，响应优先使用空 path 的 `skill_catalog_preflight_cleanup_unconfirmed`，但保留实时旧 R06 原因；标记写入故障后遗留目录仍须阻断。同一 Restart correlation ID 必须标记 `source:isos` 的旧运行停止事件，以及清理失败时的 `source:isos` manager-session 事件；P07 current view 仍为旧运行。同一管理器重试保留旧原因，管理器重启后同一 run ID 则以 `stop_interrupted` 或 `stop_result_persist_failed` 恢复为 unknown。重试不得分配新候选。检查脚本验证用例结构；IsOS 单元故障注入及限定范围的真实 EOS/MuJoCo 重放已通过，R04 TODO 关闭前仍须完成更广的跨所有者资格验证。完整输出分别位于 IsOS 忽略目录 `logs/test/20261102-r04-retirement-review-fix/` 与 EOS 忽略目录 `logs/test/20261102-r04-live-retirement/`。
