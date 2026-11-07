# Primitive operation 契约

[English](../../../docs/ei/southbound/primitives.md) · [南向概览](../southbound.md) · [线协议示例](../interfaces/eos.md)

EGIA 拥有 command identity，只有通过 registry 和 safety admission 后才会执行。它拒绝调用方提供的 arguments.command_id，为每次已准入调用生成一个 operation ID，并和精确准入的 capability name 一起提交。Operation ID 持久有效，也会作为 primitive.ack.command_id 回显。

Capability 按 robot ID、primitive、精确 version 和 device ID 与 Gateway discovery 合并。Schema 仅描述有效 arguments，不会独立授予 action 权限。设备级调用要求精确的已发现 device；runtime 级 action 的 device ID 为空。

SubmitOperation 可能返回 pending operation，此时不含 accepted 和 acknowledgement。EGIA 随后使用相同 operation ID 持续 watch。只有终态 SUCCEEDED 且匹配 acknowledgement 的 result.status 为 succeeded 才算成功。Rejected、failed、cancelled、timed_out 和 outcome_unknown 均非成功。Pending、身份不匹配，或终态与 acknowledgement 不一致时，绝不视为成功。

EGIA 将 submit 限制在五秒内，将 terminal watch 限制在三十秒内。响应丢失或超时代表结果不确定：client 不会自动再次提交相同 action，也不会为其生成替代 ID。OUTCOME_UNKNOWN 对这次观察是终态，同样不会触发重试。应基于新状态 reconciliation 或请操作员处理。

只有 EOS 声明可取消的 action kind 才能显式调用 CancelOperation，并使用原 ID；EGIA 最多等待五秒获取 terminal cancellation result。取消本地 gRPC await 本身不会停止物理动作。

**相关页面：** [EOS 线协议交换](../interfaces/eos.md)、[safety](../core/safety.md) 和 [mission recovery](../core/missions.md)。
