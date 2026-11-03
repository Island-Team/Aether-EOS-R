# 检查浏览器原语结果

[English](../../../docs/eos/usage/browser-control.md) · [R07 所有者契约](../interfaces/control_cn.md#r07-浏览器授权结果后续查询) · [R07 设计](../design/runtime/browser-results_cn.md)

**R07 受管理浏览器路径已合并，首个 mock 底盘原语演示通过资格验证。** 通过 IsOS 构建并选择 EOS 与 IsosUI，安装或链接已验证设备包，选择可用场景和图节点集。启动前先 Validate 并 Apply 图修订。IsOS 拥有保存字节、包锁定和活动修订；EOS 只读已应用投影，拥有实时健康和 Dispatcher 准入。MuJoCo 物理停止及更广泛的执行器控制由独立的 R06 关卡覆盖。

IsOS 报告部署运行后，检查保存与活动修订以及 EOS 实时就绪。在 EOS 面板选择一个**活动原语绑定实例**。面板显示注册类型和实例描述、唯一图目标、运动类型与单位，以及所有者报告的目标健康。草稿绑定或相同显示名不能自行授权命令。经回环 bridge 获取 EOS 单调时间，携活动保存字节修订及新的命令 ID 提交类型化 `primitive_typed` 载荷；EOS 返回该 ID 和状态前，本地保持 `sending`。浏览器不能提供私有授权或独立硬件目标。

`InProgress` 确认必须发生在 `Prepared` journal 记录持久化之后，也可能早于适配器释放。它只证明 EOS 已保留命令 ID；EOS 在确认前检查类型化运动限制。若提交回复丢失，保留该 ID 并使用受保护的只读查询；绝不再次发送运动命令。EOS 同时最多接纳八条此类浏览器提交；有效的新 ID 遇到满载时获得持久化的 `Rejected` 结果。结果待定时仍可通过 IsOS 使用 Stop。

响应为 `InProgress` 时，仅在 IsOS 仍报告该修订活动时有界轮询公开 `result` 后续查询；bridge 将其转换为受保护的私有 `primitive_result`。显示 EOS 精确状态：`Completed`、`Failed`、`Cancelled`、`Rejected`、`InProgress` 或 `OutcomeUnknown`。`Failed` 保持终态失败。修订过期、绑定或载荷无效、Origin 被禁止、bridge 故障、旧运行结果或缺少操作员授权，都须显示明确失败或不可用；不能显示为 `Completed`。通过 IsOS Logs 找当前运行的首条就绪或故障事件，再仅按命令 ID 的关联值筛选 bridge 诊断，不增加节点筛选；该事件的 `node_path` 为 null。Vision 面板通过独立只读 WebSocket 读取帧，并拒绝过期的订阅修订。

**Q01 EOS 生产端已实现；IsOS 接纳及所选版本回放待完成：**bridge 在收到有效私有响应后，把脱敏结果事件写入当前运行。已发布的浏览器命令 ID 仍接受 1–128 字节 UTF-8。IsosUI 生成的 `ui-<UUID>` ID 原样写入 `correlation_id`；其他有效 ID 按[事件契约](../interfaces/events_cn.md#q01-浏览器命令关联eos-生产端已实现)映射成确定性令牌。普通轮转段仍在保留期时，查询 `GET /api/isos/v1/logs?view=current&source=bridge&correlation_id=<token>`。页面不受筛选的锚点是首条就绪或故障事件，不是命令结果。缺失日志行或收集缺口不改变 EOS Dispatcher 日志结果。

使用现有 IsOS 部署 Stop 控件停止运行，然后检查终态或未知状态及 EOS 停止事件。R06 须证明底盘/机械臂在 MuJoCo 子进程终止与回收前停止。新运行轮换授权后，不能用旧浏览器结果证明新代际状态。显式图迁移仍由 IsOS 按修订单独执行。
