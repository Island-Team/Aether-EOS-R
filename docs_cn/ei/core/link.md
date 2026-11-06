# egia.core.link

[English](../../../docs/ei/core/link.md) · [核心概览](modules.md) · [EOS 2 operation 示例](../interfaces/eos.md)

Link 为每个配置的稳定 robot alias 管理一个 EOS gRPC client。它发布完整的机身与资源 generation，读取并 watch typed world resource，按需捕获 camera frame，执行有版本的 primitive，并提供 monitor 使用的直接 stop 路径。EOS driver 和 provider 保持为内部实现。

## 初始化与刷新

一个必需 generation 依次包含 Gateway Discover、ListResources、GetStatus 和 CapabilityService.ListCapabilities。EGIA 将 capability 与已发现的 robot 及精确的 primitive/device/version binding 合并。来自其他 robot 的 capability、格式错误 identity、重复 descriptor 或冲突的 availability 都会 fail closed。只有与 ready discovery binding 匹配且可用的 capability 才能执行。

在 IsOS 托管运行中，三个携带身份的响应必须报告相同的保存图 revision 与 EOS 投影摘要，且 revision 必须等于所选 `egia/stages.json` 的 revision。所选 revision 缺失或格式错误属于启动错误。IsOS 交付的 grant 文件必须归当前用户所有、权限为 `0600` 且格式精确。强制刷新失败时，在下一次 RPC 前清除写入就绪状态；之后完整且匹配的代际才能恢复。受治理的执行提交携带保存图 revision，绝不使用投影摘要替代。

随后 EGIA 调用 SkillService.ListSkills。Skill 提供规划 metadata、schema、精确 primitive reference 和 resource template；native ABI 与 library provenance 对 EOS 外部保持私有。Skill service 暂时不可用时会降级 skill snapshot，但不会阻断机身、resource 或 primitive readiness。

## Resource 与 camera capture

ListResources 提供支持的 typed world、detection 和 speech resource。WatchResource 通知变化的 URI；Link 在初始 read 前先启动 watch，收到通知时重新读取，并定时有界重读以覆盖丢失的通知。只发布通过 schema 校验的 snapshot。Camera frame 通过 CaptureFrames 按需获取。

## Primitive operation

普通调用只有通过 registry 与 safety 准入后才会到达 Link。EGIA 按精确共享 action schema 校验 arguments；设备级 action 要求精确的已发现 device，然后提交该 capability name。Robot 级 capability 使用空 device ID。arguments 中禁止调用方传入 command_id。

EGIA 为每次已准入调用创建一个 operation_id。它是 EOS 的持久幂等键，并作为 primitive.ack.command_id 回显。Pending operation 不含 accepted 字段；Link 使用相同 ID 调用 WatchOperation，直到终态更新。只有终态 operation status 为 SUCCEEDED，且匹配 acknowledgement 的 result status 为 succeeded 时才算成功。Operation 与 acknowledgement 的 ID、accepted、primitive 和 version 必须一致。

Link 将 rejected、failed、cancelled、timed out 和 outcome unknown acknowledgement 作为非成功返回。Submission RPC 成功本身不代表物理 action 已完成。若 watch 在终态前结束，或响应 identity 不一致，Link 会抛出 transport/contract error。

仅对可取消的 action class 使用原 operation ID 显式调用 CancelOperation。Link 最多等待五秒到达 terminal cancellation boundary。超时、断连或 OUTCOME_UNKNOWN 后不会自动重新提交。此时需要基于新状态 reconciliation 或操作员介入。

Submit 响应或 Watch 流丢失时，Link 使用同一个 grant 和 ID 查询 GetOperation。只有规范且匹配的终态结果才能确定该调用；查询失败使结果保持不确定，并封锁后续写入直到重新发现。Stop 与 emergency 使用固定的 safety/monitor 路径；图中的 stage 边只承载证据，不能命令 gateway。

## Transport 状态

初始化前，依赖 discovery 的 property 会抛错，而不会返回看似权威的空数据。强制 initialize 会原子替换完整 generation。未知 resource、version、target、camera 和 skill 均 fail closed。Link error 会保留 gRPC status 与 detail，由数据源 owner 负责 stream reconnection，关闭时释放 channel。

**相关页面：** [南向数据流](../southbound.md)、[registry](registry.md)、[safety](safety.md) 和 [EOS 兼容记录](../../island/eos-contracts.md)。
