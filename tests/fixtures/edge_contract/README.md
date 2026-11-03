# G06 edge contract fixture plan / G06 连线契约夹具计划

The two frozen JSON fixtures exercise EOS validation of IsOS-owned graph bytes. `valid_bindings.json` has SHA-256 `0d04df66cd20364badbf0656bfc4c03afe0cda9c2866e9a27a70aa48821db2d5`; `valid_evidence.json` has SHA-256 `51cac1fa25da50c2221dbe737e333d9d53824cfb62df06e63a3b52cb940ff74e`. Tests clone them in memory for negative cases. No fixture grants a runtime command route. IsOS G06 will reuse these cases to return JSON pointers in Apply responses. 两个固定 JSON 文件只供 EOS 校验，不改写 IsOS 图字节；负面用例在内存中修改副本。IsOS G06 将复用这些用例返回 Apply 的 JSON 指针。

| Fixture | EOS outcome | IsOS diagnostic pointer |
| --- | --- | --- |
| `valid_evidence.json` | Camera frame to EOS gate is an `eos_local` route candidate; priority `-20` and positive frequency are accepted only on that eligible camera frame edge. The gate-preview-to-Vision bridge remains an IsOS-owned G06 fixture after its `@v0.1` descriptor is integrated. | — |
| `valid_bindings.json` | Generic pinned base and manipulator each receive exactly one matching primitive binding. Both are fixed Dispatcher governed paths without user QoS. | — |
| In-memory type mutation | Source and input type IDs differ despite compatible-looking unit labels; no implicit conversion. | `/cores/eos/layers/interface/groups/gate/nodes/front/inputs/frame` |
| In-memory transport mutation | Mismatched EOS-local port transports are rejected. IsOS separately tests cross-core bridge declarations. | Exact receiving source pointer. |
| In-memory cardinality mutation | A `one` input with two sources, empty `many`, repeated source, or missing required input fails separately. A one-element array remains compatible with legacy graphs. | Exact input pointer. |
| In-memory QoS mutation | `priority` outside `-20..19`, nonpositive/nonfinite `frequency_hz`, or either field on a command/safety/result/health path fails. | Exact QoS field pointer. |
| In-memory command mutation | A contributed command-shaped source to a hardware command input fails before runtime construction. | Exact hardware command source pointer. |
| In-memory cycle mutation | EOS-owned typed edge cycles and EOS references report distinct failures; IsOS separately tests process dependency cycles. | Exact edge or reference pointer. |

G06 tests assert the classification and rejection for each case. R01 later tests fake-clock caps, `20 - priority` scheduling, bounded latest-image and FIFO event queues, gap counters and stop; those runtime claims are intentionally outside this fixture set.
