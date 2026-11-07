# EOS 世界接口

[English](../../../docs/ei/southbound/world.md) · [南向接口概览](../southbound.md)

EOS 仅通告当前可读的语义资源。EGIA 识别三类资源：

| URI | 契约 | EGIA 用途 |
|---|---|---|
| `robots/{robot}/world/state` | `world.snapshot@1` | 融合实体与关系 |
| `robots/{robot}/detection/state` | `detection.snapshot@1` | 单次模型结果及带类型的 `human:*`、`obj:*` 身份 |
| `robots/{robot}/world/asr/state` | `asr.snapshot@1` | 已识别的外部语音 |

`WatchResource` 仅报告变化的 URI。EGIA 先打开监视再执行首次读取，校验每次完整重读，合并相同快照，并在运行时作用域路由消失后通过有界重新发现进行重连。缺失资源保持不可用；EGIA 不伪造空感知。

世界与检测含义不同。世界状态可随时间融合观测，检测则保留生产设备/模型来源与 `source_sequence`。检测属性是受 Schema 约束的语义事实；原始张量、掩码、检测框、关键点、嵌入、音频及供应商载荷留在 EOS 内部。

相机图片不是可监视资源。`CaptureFrames` 返回一个 `vision.snapshot@2`，包含一至两张有序 JPEG 图片及尺寸和 RFC3339 `capturedAt` 时间戳。EGIA 在使用前校验信封、请求数量、JPEG 结构、尺寸、时间戳及唯一性。原始麦克风音频不会跨越接口。
