# World 与感知资源

[English](../../../../docs/eos/develop/interfaces/world.md) · [接口总览](README_cn.md) · [Body](body_cn.md) · [Primitive](actions_cn.md)

World 描述机器人之外的动态物理事实。机器人本体的设备、primitive、健康、执行器状态和电量只在 [Body](body_cn.md) 中出现。当前可读取的 world 产品包括 semantic detection、VAD 和 ASR；外部融合 `world.snapshot` schema 已保留，但当前没有 producer，也不发布 `robots/{robot}/world/state` 路由。

`robots/{robot}/detection/state` 返回一次模型结果的 `detection.snapshot`；`robots/{robot}/world/vad/state` 返回一个麦克风窗口的紧凑语音活动状态；`robots/{robot}/world/asr/state` 返回识别出的外部语音。已配置的 retained resource 可在首个输出前出现在 `ListResources`；cold read/watch 会按需启动共享 producer 并等待有效产品。`ReadResource` 返回完整快照，`WatchResource` 只报告发生变化的 URI。

```protobuf
ReadResourceRequest { uri: "robots/demo-0/detection/state" }
WatchResourceRequest { uri: "robots/demo-0/detection/state" }
```

## 检测与融合状态

检测是一轮模型推理的证据；融合 world 状态未来可结合多轮观测。Track ID 使用 `human:<track>` 或 `obj:<track-or-domain-id>`，便于在检测和未来的融合状态间关联。检测结果保留语义标签、置信度、来源与可用的距离和方向等紧凑字段，不包含 mask、keypoint 或 tensor。例如：

```json
{"type":"detection.snapshot","schema_version":"1.0","sequence":42,"timestamp":"2026-09-20T09:30:01Z","source":"cam-0","source_sequence":317,"source_generation":317,"source_frame_id":"camera_optical","source_timestamp_ns":1789983000000000000,"depth_available":true,"face_recognition_status":"complete","detections":[{"id":"human:7","kind":"human","label":"person","confidence":0.94,"distance_m":2.1,"bearing_rad":-0.12}]}
```

保留的 `world.snapshot` 用 `entities` 和 `relations` 描述跟踪的物理实体，缺失的可选事实应省略。它不等同于检测框，也不能被当成当前可读资源。完整字段和示例见 [英文接口](../../../../docs/eos/develop/interfaces/world.md)。

## 按需图像、VAD 与 ASR

原始 RGB 只通过受限的 `CaptureFrames` 按需获取，不是可读取或监听的资源。Vision snapshot 与 detection snapshot 分开；客户端不应通过检测资源寻找帧 bytes。

VAD 只报告语音活动、概率和来源，不包含 PCM、WAV、完整 utterance、segment 或 provider metadata。ASR 只报告识别文本和相关紧凑 provenance，不包含音频。读取这些状态时应检查时间戳与序列，避免把旧输出当成当前感知。
