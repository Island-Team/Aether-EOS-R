# 技能社区

[总览](../../../design/runtime/skills_cn.md) · [进程模型](../../../design/runtime/skills/process-model_cn.md) · [ABI 参考](abi_cn.md) · [English](../../../../../docs/eos/interfaces/skills/native/community.md)

**技能社区**是由 `SkillManager` 拥有的应用级通信机制。打包的 ABI v2.1 技能使用可选的 `community_call` host callback；EOS 将隔离 worker 的调用桥接到管理器。内置技能继续使用原有的父子 `ProcessHandle` 邮箱 API。社区消息在内部复用现有的有界 worker 传输；调用方不依赖 Linux IPC 或 DDS。

技能描述符为每个 channel 声明 object JSON Schema 及 `can_send` 和/或 `can_receive`。loader 拒绝无效或重复声明。原生执行器和管理器在每次调用时检查方向；定向发送会同时按发送方和接收方的 schema 校验 payload。用于发布/接收的 channel 名也是命名主题。主题发布可送达所有具有匹配接收权限且 schema 接受该对象的其他活动执行，即使执行之间没有亲属关系。发送方不接收自己的发布。

## 操作与交付

`EOS_SKILL_COMMUNITY_SEND` 发送到一个活动执行 ID，并返回是否新入队。对于该接收方，保留期内相同的 `(sender, request_id)` 和内容返回 `sent: false`；冲突复用失败。此定向操作允许无亲属关系的执行。内置技能原有的 `ProcessHandle` `send` 操作仍要求父子关系。

`EOS_SKILL_COMMUNITY_PUBLISH` 尝试一次有界扇出，返回主题序列及送达和丢弃计数。接收方邮箱已满时增加其丢弃计数，其他接收方仍继续接收。schema 不兼容的接收方被跳过。发布并非面向所有接收方的原子事务，也不会重试丢失消息。

`EOS_SKILL_COMMUNITY_RECEIVE` 最多等待指定时长，取出其已声明 channel 中第一条保留消息；超时返回 `null`。每次取出一条。每个接收方邮箱有自己的序列；`dropped_before` 是该消息之前该邮箱累计丢失的消息数。主题发布另有逐主题序列。原始社区 callback 不实现请求/回复事务；调用方须自行关联回复。

ABI 请求使用 `operation`、定向发送时的 `recipient_execution_id`、`channel`，以及发送或发布时的 object `payload_json`、`request_id` 和 `timeout_ms`。发送和发布将 timeout 解释为正数 TTL；接收将其解释为等待时间。发送/发布的 TTL 最多一小时，request ID 为 1–96 字节，序列化 payload 最多 16 KiB。管理器默认每个执行最多保留 256 条待收消息。定向发送可最多等待其 TTL 以获取空间；发布不会等待已满的接收方邮箱。过期消息被清理并计入丢弃。取消、期限、端点失活、无效 schema 或权限、队列耗尽均会明确失败或结束调用。

对于已声明 `task.update` 发送权限的技能，原始 host 调用可发布一个 object：

```c
eos_skill_community_request_v2 request = {0};
request.struct_size = sizeof(request);
request.operation = EOS_SKILL_COMMUNITY_PUBLISH;
request.channel = (eos_skill_text_v2){"task.update", sizeof("task.update") - 1};
request.payload_json = (eos_skill_text_v2){"{\"state\":\"ready\"}", sizeof("{\"state\":\"ready\"}") - 1};
request.request_id = (eos_skill_text_v2){"update-4", sizeof("update-4") - 1};
request.timeout_ms = 1000;
eos_skill_community_response_v2 response = {0};
response.struct_size = sizeof(response);
if (EOS_SKILL_HOST_V2_HAS(host, community_call) && host->community_call != NULL)
  host->community_call(host->context, &request, &response);
```

例如，主题发布成功结果可为：

```json
{"sequence":4,"delivered":2,"dropped":1}
```

一次接收结果可为：

```json
{"message_id":"0192a000-0000-7000-8000-000000000001","request_id":"update-4","correlation_id":"update-4","sender_execution_id":"sender-1","channel":"task.update","topic_message":true,"sequence":7,"dropped_before":1,"payload":{"state":"ready"}}
```

响应中借用的 `result_json` 和 `error_message` 仅在下一次社区调用前有效。原始 C 调用方先检查 `EOS_SKILL_HOST_V2_HAS(host, community_call)` 和函数指针；缺失表示此可选 ABI v2.1 功能不可用。C++ 作者可通过 `SkillBuilder::Channel(name, schema, can_send, can_receive)` 声明 channel，并调用 `Execution::send_community(recipient_execution_id, channel, payload, request_id, ttl)`、`publish_topic(topic, payload, request_id, ttl)` 或 `receive_message(channel, timeout)`。返回值依次为 `Result<bool>`、`Result<TopicPublish>` 和 `Result<std::optional<CommunityMessage>>`；SDK 会复制借用的响应，并在 callback 缺失时报告 unavailable。

社区邮箱和主题计数器只存在于进程内，重启后不恢复。它们不生成持久工作流事实、日志、全局顺序、重放或隐式送达保证。`dropped_before` 暴露邮箱缺口；新启动意味着新的通信代次。公开 Agent SIGNAL 仍是独立的技能控制表面。[worker 边界](../../../design/runtime/skills/external_cn.md)说明内部传输及取消行为。
