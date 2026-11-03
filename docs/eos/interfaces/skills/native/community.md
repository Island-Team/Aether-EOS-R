# Skill community

<!-- docs-tree:start -->
**Location:** [Docs](../../../../README.md) / [EOS](../../../README.md) / [Interfaces](../../README.md) / [Skills](../README.md) / [Native](README.md) / **community**

<details>
<summary>Docs tree</summary>

- [docs/](../../../../README.md)
  - [eos/](../../../README.md)
    - [interfaces/](../../README.md)
      - [skills/](../README.md)
        - [native/](README.md)
          - [abi.md](abi.md)
          - [build-install-test.md](build-install-test.md)
          - [builtin.md](builtin.md)
          - [capabilities.md](capabilities.md)
          - **community.md**
          - [example.md](example.md)
          - [package-abi.md](package-abi.md)

</details>
<!-- docs-tree:end -->

[Overview](../../../design/runtime/skills.md) · [Process model](../../../design/runtime/skills/process-model.md) · [ABI reference](abi.md) · [中文](../../../../../docs_cn/eos/interfaces/skills/native/ipc_cn.md)

The **skill community** is application-level communication owned by `SkillManager`. A packaged ABI v2.1 skill uses the optional `community_call` host callback; EOS bridges that call from its isolated worker to the manager. Built-in skills retain their older parent/child `ProcessHandle` mailbox API. Community messages use the existing bounded worker transport internally; callers do not depend on Linux IPC or DDS.

Every channel is declared on the skill descriptor with an object JSON Schema and `can_send` and/or `can_receive`. The loader rejects invalid or duplicate declarations. The native executor and manager check direction on each call; addressed delivery validates the payload against both sender and recipient schemas. A channel used for publish/receive acts as a named topic. A publication reaches every other active execution with matching receive authority and a schema that accepts the object, even when the executions are unrelated. The sender does not receive its own publication.

## Operations and delivery

`EOS_SKILL_COMMUNITY_SEND` sends to one active execution ID and returns whether a new message was queued. An identical retained `(sender, request_id)` for that recipient returns `sent: false`; conflicting reuse fails. This addressed operation allows unrelated executions. The older built-in `send` operation using a `ProcessHandle` still requires a parent/child relationship.

`EOS_SKILL_COMMUNITY_PUBLISH` attempts one bounded fan-out and returns its topic sequence plus delivered and dropped counts. A full recipient mailbox increments its drop count; delivery to other recipients continues. A recipient with incompatible schema is skipped. Publication is not an atomic all-recipient transaction and does not retry dropped messages.

`EOS_SKILL_COMMUNITY_RECEIVE` waits up to its timeout for the first retained message on its declared channel and returns `null` on timeout. It consumes one message. Each recipient mailbox has its own sequence; `dropped_before` is the cumulative number of messages lost from that mailbox before the returned message. Topic publication has a separate per-topic sequence. The raw community callback does not implement a request/reply transaction; callers must manage any reply correlation themselves.

The ABI request uses `operation`, `recipient_execution_id` for addressed send, `channel`, object `payload_json` and `request_id` for send or publish, and `timeout_ms`. Send and publish interpret the timeout as a positive TTL; receive interprets it as a wait duration. A send/publish TTL is at most one hour, request IDs contain 1–96 bytes, and serialized payloads are at most 16 KiB. The manager's default mailbox capacity is 256 pending messages per execution. An addressed send can wait up to its TTL for space; publish does not block on a full recipient mailbox. Expired messages are pruned and counted as drops. Cancellation, deadlines, inactive endpoints, invalid schema or authority, and queue exhaustion end or fail the call explicitly.

For a skill with send authority for `task.update`, the raw host call can publish one object:

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

For example, successful result JSON for a topic publication can be:

```json
{"sequence":4,"delivered":2,"dropped":1}
```

A receive result can be:

```json
{"message_id":"0192a000-0000-7000-8000-000000000001","request_id":"update-4","correlation_id":"update-4","sender_execution_id":"sender-1","channel":"task.update","topic_message":true,"sequence":7,"dropped_before":1,"payload":{"state":"ready"}}
```

The response's borrowed `result_json` and `error_message` remain valid only until the next community call. Raw C callers check `EOS_SKILL_HOST_V2_HAS(host, community_call)` and the callback pointer before invoking it; absence means this optional ABI v2.1 feature is unavailable. C++ authors can declare a channel with `SkillBuilder::Channel(name, schema, can_send, can_receive)` and call `Execution::send_community(recipient_execution_id, channel, payload, request_id, ttl)`, `publish_topic(topic, payload, request_id, ttl)`, or `receive_message(channel, timeout)`. These return `Result<bool>`, `Result<TopicPublish>`, and `Result<std::optional<CommunityMessage>>` respectively; the SDK copies the borrowed response and reports a missing callback as unavailable.

Community mailboxes and topic counters are process-local and are not restored on restart. They do not create durable workflow facts, a journal, global ordering, replay, or implicit delivery guarantees. `dropped_before` exposes mailbox gaps; a new boot starts a new communication epoch. Public Agent SIGNALs remain a separate skill-control surface. See the [worker architecture](../../../design/runtime/skills/external.md) for internal transport and cancellation.
