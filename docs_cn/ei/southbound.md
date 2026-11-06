# EOS 南向接口

[English](../../docs/ei/southbound.md) · [机身](../../docs/ei/southbound/body.md) · [世界](../../docs/ei/southbound/world.md) · [Primitive](../../docs/ei/southbound/primitives.md) · [Skill](../../docs/ei/southbound/skills.md) · [EOS 2 线协议示例](../../docs/ei/interfaces/eos.md)

EGIA 通过 native gRPC 使用 EOS Gateway discovery 与 resource、EOS 2 CapabilityService operation API 和 SkillService。EOS 拥有 protobuf 与 JSON Schema 源；它们由经过冲突检查的共享资源 root 选定。EGIA 拥有生成的 Python protobuf binding、typed adapter、安全策略、认知处理和面向操作员的 projection。EGIA 不使用 EOS HTTP/ProtoJSON listener。

生成契约固定到 Island-eos 提交 c43e5a32eafd9bf22b2dba2940662dcded60111c。该提交是开发提交，不是 EOS release tag。精确 pin 见 [EOS 兼容性](../island/eos-contracts.md)，线协议流程见 [operation 示例](interfaces/eos.md)。

启动需要 Gateway discovery、resources、status、capability 以及规范 schema 清单。EGIA 根据已发现的 robot 和精确 primitive/device/version binding 校验每项 capability。格式错误或属于其他 robot 的 peer capability 会被拒绝；schema 本身不会授予执行权限。

## 数据流

\`\`\`mermaid
flowchart LR
  discover["Gateway discovery"] --> generation["Atomic robot/device generation"]
  capabilities["CapabilityService catalog"] --> binding["Exact ready capability join"]
  generation --> binding
  resources["Read/watch resources"] --> typed["Canonical schema validation"]
  binding --> registry["Executable registry"]
  typed --> perception["World + detection + ASR admission"]
  registry --> safety["EGIA safety gate"]
  safety --> operation["SubmitOperation with operation_id"]
  operation --> terminal["WatchOperation terminal acknowledgement"]
  interrupt["Timeout or cancellation"] --> cancel["CancelOperation by operation_id"]
  cancel --> terminal
  terminal --> mission["Mission outcome + structured log"]
\`\`\`

每个可执行 binding 必须健康、匹配安装的精确版本 schema，并通过本地 policy。异常 binding 会保留用于诊断，但不可执行。刷新会原子替换完整 generation。

## 接口分组

- [机身](../../docs/ei/southbound/body.md)介绍 robot/device identity、状态、health、binding scope、rediscovery 和 status。
- [世界](../../docs/ei/southbound/world.md)介绍 typed world state、detection、speech、resource watch 和 camera capture。
- [Primitive](../../docs/ei/southbound/primitives.md)介绍 action version、持久 operation identity、终态、显式取消和不确定结果。
- [Skill](../../docs/ei/southbound/skills.md)介绍专用 skill service 提供的 descriptor 与异步执行。

## 契约所有权与兼容性

EOS 拥有 proto 与 JSON Schema 源。scripts/generate_eos_proto.py 从固定提交生成 EGIA 检入的 Python projection；不得手工修改生成文件。契约变更应先更新 EOS 并记录精确提交，再重新生成 binding，之后更新 typed consumer、测试和双语文档。参见[契约校验](../island/contracts.md)。
