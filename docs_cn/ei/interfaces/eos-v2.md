# EOS 2 能力操作

[English](../../../docs/ei/interfaces/eos-v2.md) · [Link transport](../core/link.md) · [EOS 共享契约](../../island/eos-contracts.md)

EGIA 从 Island-eos 提交 c43e5a32eafd9bf22b2dba2940662dcded60111c 固定并生成 EOS protobuf binding。该提交对应开发契约，尚不是公开 EOS release tag。修改固定提交后，使用 scripts/generate_eos_proto.py 重新生成；禁止手工修改生成文件。

## 能力身份与准入

EGIA 先发现机器人，再列出能力。能力名称必须属于已发现的机器人，并与 descriptor 中的 primitive 和 version 一致。EGIA 只会把能力绑定到 EOS discovery 已有的 primitive，且要求 device 与 version 完全匹配；公告的别名不会创建新权限。能力名称格式如下：

```text
hardware/{robot_id}/{device_class}/{part_id}/{primitive}@{version}
runtime/{robot_id}/{primitive}@1.0.0
```

例如：

```text
hardware/robot-123/base/base-0/base.velocity@1.0.0
runtime/robot-123/robot.stop@1.0.0
```

第一种是设备能力。runtime 格式是进程级能力，device_id 为空。来自其他机器人的目录项属于格式错误的 peer 响应，会导致 discovery 失败。如果多个名称指向同一个 action target，EGIA 仅在 availability 一致时接受，并确定性地选择一个名称。

## Operation 线协议交换

### IsOS 托管代际与授权

在受监督的托管运行中，IsOS 只向 EGIA 传递 `EGIA_EOS_CONTROL_GRANT_FILE`。该值是绝对路径，指向归当前用户所有、权限为 `0600` 的普通文件，内容恰好是 64 个小写十六进制字符和一个换行。EGIA 不记录 grant。IsOS 渲染的 `egia/stages.json` 以恰好 64 个小写十六进制字符携带所选保存图 `revision`。发布代际前，Discover、GetStatus 与 ListCapabilities 的 gRPC 响应 metadata 必须含一致的 `eos-active-revision` 和 `eos-projection-sha256`，且 **active revision 必须等于所选覆盖文件的 revision**。两项 metadata 均为小写 SHA-256；前者标识 IsOS 保存的完整图，也是 `x-eos-active-revision` 唯一可用的值。投影摘要仅用于诊断，不能替代它。代际混杂、缺失、格式错误、内部一致但属于错误保存图、所选 revision 无效或 grant 文件不安全时，写入保持关闭，直到完成新的匹配发现。强制刷新在首个 RPC 前就封锁旧代际。

SubmitOperation、GetOperation、WatchOperation 与 CancelOperation 均携带 `authorization: Bearer <grant>`。移动、`motion.stop` 和 `robot.stop` 的 Submit 还携带保存图的 `x-eos-active-revision`；已认证的 `safety.estop` 可以省略它。查询、监视和取消使用同一持久角色与 operation ID，不要求当前 revision。固定的 decision → safety → link 和 monitor 紧急路径由代码拥有；图边不能提交 EOS 命令，也不能绕过 EOS Dispatcher 准入。没有托管 metadata 的独立 EOS 对托管 EGIA 仅提供只读发现。

operation_id 是持久幂等键。EGIA 为每次已准入调用生成一个 UUID，并在 SubmitOperationRequest 中发送；同一个物理动作重试时绝不能生成新 ID。能力名称必须是精确的已准入 descriptor；传输前会根据匹配的共享 action schema 校验 protobuf Struct arguments。

Request:

```json
{
  "capability_name": "hardware/robot-123/base/base-0/base.velocity@1.0.0",
  "arguments": {
    "linear_x_m_s": 0.1,
    "linear_y_m_s": 0.0,
    "angular_z_rad_s": 0.0,
    "valid_for_ms": 100
  },
  "operation_id": "75a852c0-894a-4d93-b1b9-31fd642621cd"
}
```

Pending operation 不含 accepted 字段，也不含终态 acknowledgement：

```json
{
  "operation_id": "75a852c0-894a-4d93-b1b9-31fd642621cd",
  "status": "OPERATION_STATUS_PENDING"
}
```

EGIA 使用同一个 operation ID 调用 WatchOperation，直到收到终态更新。成功终态同时携带准入结果和规范 primitive.ack：

```json
{
  "operation_id": "75a852c0-894a-4d93-b1b9-31fd642621cd",
  "status": "OPERATION_STATUS_SUCCEEDED",
  "accepted": true,
  "acknowledgement": {
    "type": "primitive.ack",
    "primitive": "base.velocity",
    "primitive_version": "1.0.0",
    "accepted": true,
    "reason": null,
    "reason_code": null,
    "command_id": "75a852c0-894a-4d93-b1b9-31fd642621cd",
    "sequence": 42,
    "result": {"status": "succeeded"}
  }
}
```

只有 acknowledgement 中的终态 SUCCEEDED 且 result 为 succeeded，并且 operation ID、primitive、version 和 accepted 值均匹配时才算成功。终态与 acknowledgement 不一致属于协议错误。Rejected、failed、cancelled、timed out 和 outcome unknown 都不代表成功。

## 取消与不确定结果

取消是显式的 CancelOperation 请求，并使用相同的持久 operation ID：

```json
{"operation_id": "75a852c0-894a-4d93-b1b9-31fd642621cd"}
```

EGIA 只对 EOS 声明可取消的 action class 请求取消，并继续 watch 到终态边界。非可取消 action 被中断或超时后不会重放。OUTCOME_UNKNOWN 对当前观察是终态，绝不能自动再次提交；应根据新状态 reconciliation 或请求操作员处理。

SubmitOperation 丢失响应或 WatchOperation 断开时，EGIA 使用**同一个 ID**通过已认证 GetOperation 查询。只有身份匹配的终态 acknowledgement 才能确定结果；不存在、已封存、未授权、长期 pending 或不一致的结果仍不确定。EGIA 不会第二次提交该物理动作。传输中断会使已发布代际失效，下一次写入必须重新完成发现。

传输层不会将成功的 gRPC submission 当作物理执行成功。响应必须已经处于终态，或由 watch stream 返回身份匹配的终态 operation。每个响应和更新都会校验 operation identity。

## 验证

Stub contract 测试覆盖 robot identity、精确准入绑定、command identity、pending 到 terminal 流程、取消、错误 acknowledgement，以及不重试不确定结果。公开发布还要求针对某个精确公开 EOS release 完成真实 EOS process 集成测试。
