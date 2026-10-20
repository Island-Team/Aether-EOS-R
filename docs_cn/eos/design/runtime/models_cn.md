# 模型运行时

[运行时总览](README_cn.md)

`src/eos/runtime/models/` 负责私有 capability schema、规范请求/响应校验、有界推理 worker、HTTP 与 vision WebSocket transport，以及健康轮询。Provider 字段映射和响应归一化位于 `src/eos/ext/models/`；部署可增加 adapter， 而无需把 provider 名称写入 core runtime。Agent-facing gateway 只公开 VAD、human、human-pose 与 face-recognition 四个 typed one-shot call，不公开 generic model method 或详细模型状态。

## Capability 与私有契约

runtime 通过精确逻辑 model ID 支持 audio VAD、transcription、synthesis、generic object detection、tracked human、完整 human pose 与 face recognition。其 schema 位于 `share/contracts/eos/external/models/schemas/`，不属于 shared manifest。

`Runtime::Call(model_id, request)` 校验规范请求，通过 HTTP 或持久 vision WebSocket 调用 provider， 归一化响应并校验规范结果。WebSocket transport 当前支持 object detection、tracked human、human pose 与 face recognition。每个已配置 WebSocket model 保留一条 connection，且同时只有一组 request/response exchange；replace-latest camera worker 提供有界 backpressure，不依赖 provider-side response multiplexing。`provider_model_id`、`file_field`、静态 `fields` 与 `api_key_env` 都是 transport/adapter 配置；core 不把 provider 专用字段注入规范调用。

ASR、TTS 与 VAD 请求可带 `stream`，省略等价于 `false`。当前调用 API 是 buffered；`stream = true` 会明确失败。Human-pose 可带与 RGB 像素对齐的无损 depth image 及 raw-unit-to-metre scale；multipart adapter 将 depth image 编码为 `depth_file`。

当前只支持 `wire_format = "json"` 或 `"multipart"`。Provider 差异通过 `adapter` 选择；例如 `legacy-vision`、`multipart-asr` 和 `audio8-speech-v1`。不存在 `request_profile`、 `wire_format = "local"` 或 `[perception] local_fallback`，EOS 也不会在服务失败时静默替换为进程内模型。

## 自动硬件链路

`CameraPosePump` 读取 camera telemetry 后立即发布每个 source frame，再异步调用配置的 `vision.human` 模型。Human、pose、action 与 face 各自维护 `pending`、`complete` 或 `unavailable` 状态、sequence 与 source generation。独立 replace-latest worker 提供有界 backpressure，因此任何模型都不会阻塞 source 或其他模型结果的发布；迟到结果仍只关联原始 generation。Camera store 只接受 provider 标记为 known 且 similarity 至少为 `0.40` 的注册身份；只要同一个显式且唯一的 track ID 仍然存在，它就会忽略 bbox 移动，在所有后续空、unknown 或低 similarity 结果中保留该身份。不同 track、重复 ID 或目标消失都会清除缓存身份；较新的合格身份会立即替换旧身份。Pose enrichment 只传递 detection 的 `track_id`、`bbox`、`confidence` 与 optional `distance_m`，不把其他已归一化状态回送给 provider。`CameraObjectPump` 通过 HTTP 或 WebSocket 调用配置的 `vision.object_detection`；read、watch 或 skill subscription 会按需启动共享 pump，并发布经过校验的 head-detection history。`MicrophoneVadPump` 接收 16 kHz 单声道 PCM window，调用精确 `audio.vad` 模型， 把语音片段合并为 utterance，然后尝试精确模型 ID `asr`，并把成功的 VAD/ASR 结果发布到 `MicrophoneVadStore`。Perception Reader 读取这些 store；这些 pump 都不会创建 northbound transport 或 释放硬件命令。

每次成功 VAD call 都会成为一个经校验、有界的 `vad.snapshot`，保留在 `robots/{robot}/world/vad/state`。read 与 watch demand 与 continuous stream 共享同一 `microphone_vad` producer。PCM、generated WAV、utterance audio、model segment 与 provider metadata 不进入该 history。

连续链路只由各自的 `[perception.stream]` 开关启用；所有开关默认 `false`，已提交配置只启用 `vad` 与 `human`。`human`、`human_pose` 或 `face` 中任一开关都会启动 camera human producer，后两者还会启用各自的 enrichment worker。`vad` 使用 `microphone_vad.model_id`，所有 camera 开关都校验 `camera_pose.detection_model_id`，`human_pose` 使用 `camera_pose.model_id`，`face` 使用 `camera_face.model_id`。缺失或 false 的开关不会回退到旧 camera/VAD pump。

optional `asr` follow-up 在 call time 解析，因此缺失 ASR 不阻止 VAD pump 启动。启用 log attachment 时，提交给 ASR 的 merged WAV 以 `asr-<uuid>.wav` 存入 flat launch attachment directory，并由 `microphone_asr_requested` 引用；不存在 `asr/` 子目录。旧 `save_audio` 设置仍独立地把 diagnostic utterance WAV 写入 microphone VAD state directory。

Face v1 schema 保留 legacy provider operation shape。Typed northbound 与 skill inference 只构造 `recognize`；其他 operation 中 EOS 只使用 registration，且 dispatcher 仅通过独立校验的内部 `Runtime::RegisterFace` 为 `face.register` primitive 执行它。

Skill 的 one-shot vision inference 不需启用 continuous stream。它可直接提交 canonical inline `input`（及 capability 支持的 depth/options），或指定 bounded camera store 中的精确 frame；两种模式互斥。`Runtime::Call` 对两者都强制 request bound、schema、response invariant 与 cancellation。

硬件 binding 也可声明模型支持的 advanced primitive。`[[device.primitive]] model_id` 选择一个精确 模型，不按 capability 搜索替代项。Advanced primitive 属于已发布命令面，因此模型必须存在、启用并匹配 catalog capability，否则 EOS 拒绝启动。`speaker.speak@1.0.0` 当前以此方式绑定 `audio.synthesis`。通过完整 PCM WAV 检查后，输出以 `tts-<uuid>.<ext>` 存入 flat launch attachment directory，并在释放给 speaker backend 前由 `tts_audio_generated` 引用。

## 健康与调用边界

模型可配置独立 HTTP `health_endpoint`；`health_interval_ms` 和 `health_timeout_ms` 控制独立于 inference worker 的共享双 worker 健康调度。单个模型不会重叠 probe，关闭 runtime 会协作取消 active probe。 未配置 endpoint 时，健康为 unavailable，并只产生一次配置 warning。普通 `Runtime::Call` 不因缓存健康状态拒绝调用，但 advanced `speaker.speak` 要求其精确 TTS 依赖报告 ready。 `GetStatus` 只公开 enabled 模型的 total/ready/checking/degraded/unavailable 聚合计数，不公开 endpoint、 credential、header 或 provider metadata。

配置在进程生命周期内不可变。`enabled` 默认为 `true`；禁用条目仍完整校验并参与 ID 去重，但不会被调用。 模型调用只允许 numeric IPv4/IPv6 host 的明文 `http://` 或 `ws://` endpoint；需要 DNS 或 TLS 的部署应使用本地代理。 请求与响应分别受 `max_request_bytes` 和 `max_response_bytes` 限制，connect/write/read 受 `timeout_ms` 限制。模型调用不会在周期性控制 loop 中执行。

## 当前参考配置

提交的 `etc/eos/eos.example.toml` 声明八个逻辑客户端。Human 与 human-pose 使用持久 WebSocket，ArcFace registration 使用独立 WebSocket client；ArcFace recognition、Audio8、multipart ASR 与 VAD 使用 HTTP。配置使用 `adapter` 隔离 provider 字段映射；即使多个 capability 共用物理服务，逻辑 ID 仍保持精确且稳定。

## 测试

模型测试使用 loopback provider 覆盖配置、JSON/multipart 编码、adapter 映射、请求/响应上限、deadline、 非 2xx、malformed payload、健康恢复、并发与 clean shutdown。自动链路测试覆盖相机 pose、对齐 depth 及 麦克风 VAD/ASR 输入准入。
