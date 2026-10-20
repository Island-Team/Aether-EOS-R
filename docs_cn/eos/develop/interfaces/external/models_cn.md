# 内部模型接口

[English](../../../../../docs/eos/develop/interfaces/external/models.md) · [接口总览](../README_cn.md) · [模型运行时](../../../design/runtime/models_cn.md)

EOS 通过私有的 capability 接口调用模型，不把 provider URL、header、凭据和原生载荷传到 EOS 面向客户端的 gRPC 接口。`Health(model_id)` 返回规范健康记录；`Call(model_id, request)` 校验请求、调用配置的服务、归一化响应并返回结构化状态。规范 schema 位于 `share/contracts/eos/external/models/schemas/`，不属于 `share/ext/eos/` 的公开契约清单。

配置的 provider 传输可为 HTTP、WebSocket 或 gRPC。同机 gRPC provider 优先使用 Unix 域套接字，也支持 TCP target。`share/contracts/eos/external/models/model_provider.proto` 中的私有 `island.eos.external.models.v1.ModelProviderService` 提供一元 `Health` 和 `Call`。配置 `transport = "grpc"`，并把 `endpoint` 设为 `"unix:/path.sock"` 或 `"127.0.0.1:port"`；省略 HTTP 的 `health_endpoint`。`Call` 携带配置的 provider model ID、capability 与规范 UTF-8 JSON 字节，按照其他传输共用的能力请求与响应 schema 校验。调用受截止时间、取消和消息大小限制；该缓冲接口不增加增量流式输出。

高级硬件 primitive 在 profile 的 `[[device.primitive]]` 中绑定一个精确的 `model_id`。依赖模型必须启用且具有对应 capability，否则 EOS 拒绝启动；不会用同 capability 的另一个模型替换。此绑定不改变 公开 primitive 契约。

## 阅读示例

尖括号占位符需整体替换为对应 base64 数据。可选字段只在有助于说明时出现。`_s` 表示秒；置信度和概率在 0 到 1 之间；图像坐标以左上角为原点，单位是像素；`bbox` 为 `[x1, y1, x2, y2]`；count 必须等于数组长度。未知字段会被拒绝，不会转发给 provider。

## 健康状态

已启用的 HTTP 模型可以配置健康端点。EOS 在专用 worker 中探测并归一化；初始状态为 `checking`，后续有效探测可以使其恢复。健康状态仅供观察，探测不健康不阻止推理。未配置端点时保持 unavailable，并只发出一次配置警告。Provider 状态归一化为 `ready`、`degraded` 或 `unavailable`；身份或 capability 不匹配、格式错误和超时均归为 unavailable。

```json
{
  "interface_version": "1.0",
  "status": "ready",
  "service": {
    "name": "inference-service",
    "version": "1.4.0"
  },
  "model": {
    "id": "primary-model",
    "version": "2.1.0"
  },
  "capabilities": [
    "vision.human_pose"
  ]
}
```

## 调用

每个配置模型只声明一个 capability，对应一对请求与响应 schema；EOS 在调用两侧校验，adapter 转换 provider 专用表示。ASR、TTS、VAD 的可选 `stream` 默认为 `false`。当前 `Call` 为缓冲式，只接受 `false`；`true` 明确失败。增量输出需要独立的流式调用 API 和 transport adapter。ASR 的原始音频保持私有，仅派生的 `asr.snapshot` 可投影到 northbound。

## 自动语音识别（ASR）

ASR 把音频转换为文本。请求仅要求 `input.data_b64`，还可提供 `input.filename`、`input.media_type`、`language`、`prompt`、`temperature`、`stream`。响应仅要求 `text`；`language`、`duration_s`、`segments` 和 segment 的 `confidence` 可选。

```json
{
  "input": {
    "data_b64": "<base64-encoded audio>"
  }
}
```

```json
{
  "text": "Please open the door.",
  "language": "en",
  "duration_s": 1.84,
  "segments": [
    {
      "start_s": 0.0,
      "end_s": 1.84,
      "text": "Please open the door.",
      "confidence": 0.97
    }
  ]
}
```

## 目标检测

目标检测定位并分类图像对象。请求仅要求 `input.data_b64`；`input.filename` 和 `input.media_type` 是可选传输提示，`conf` 和 `iou` 分别为置信度与交并比阈值。每个结果必须有 `label`、`confidence`、`bbox`；provider 无数值类别目录时可省略 `class_id`。

```json
{
  "input": {
    "data_b64": "<base64-encoded JPEG image>"
  },
  "conf": 0.4,
  "iou": 0.5
}
```

```json
{
  "objects": [
    {
      "label": "person",
      "class_id": 0,
      "confidence": 0.96,
      "bbox": [
        120,
        80,
        420,
        700
      ]
    }
  ],
  "object_count": 1
}
```

## 人体检测

`vision.human` 返回带跟踪 ID 的人体框。每行要求 `track_id`、`bbox` 和 `confidence`；对齐深度提供距离时保留 `distance_m`。它与基于标签的 `vision.object_detection` 不同。

## 人体姿态

`vision.human_pose` 接受图像和可选的带跟踪 `persons`，返回完整 COCO-17 关键点、姿态和行为。`persons`、`conf`、`kpt_conf` 可选。深度输入也是可选的，但 `depth_input` 必须是与 RGB 像素对齐的无损单通道深度图；原始深度乘 `depth_scale` 得到米。`depth_intrinsics` 的尺寸必须与两幅解码图匹配，含 `width`、`height`、`fx`、`fy`、`ppx`、`ppy`、`distortion_model` 和五个 `coeffs`。multipart adapter 用 `depth_file` 发送深度图，RTMPose WebSocket adapter 映射为 `depth_b64`、`depth_scale` 和 `depth_intrinsics`。深度不可用或超出字节上限时，EOS 一并省略三项，继续 RGB 路径。相机传输细节在模型边界之前已转换成 backend-neutral telemetry。响应必须有 17 个 keypoints 和 17 个 scores。`pose` 为 `stand`、`sit`、`lie`、`unknown`；`action` 为 `waving`、`fallen`、`none`、`unknown`。`distance_m` 无法估计时为 null。请求携带 `persons` 时，响应须保留对应 EOS `track_id`；EOS 只合并匹配现有人体的结果。家具上下文稍后由 world projection 处理。

```json
{
  "input": {
    "data_b64": "<base64-encoded JPEG image>"
  }
}
```

```json
{
  "persons": [
    {
      "track_id": 7,
      "bbox": [
        210,
        80,
        510,
        700
      ],
      "confidence": 0.94,
      "distance_m": 2.1,
      "keypoints": [
        [310, 120],
        [300, 110],
        [320, 110],
        [290, 115],
        [330, 115],
        [275, 210],
        [345, 210],
        [250, 320],
        [370, 320],
        [235, 420],
        [385, 420],
        [285, 410],
        [335, 410],
        [280, 540],
        [340, 540],
        [275, 675],
        [345, 675]
      ],
      "keypoint_scores": [
        0.95, 0.94, 0.94, 0.91, 0.92, 0.96, 0.96, 0.93, 0.93,
        0.90, 0.91, 0.97, 0.97, 0.95, 0.95, 0.92, 0.92
      ],
      "pose": "stand",
      "pose_confidence": 0.93,
      "action": "none",
      "action_confidence": 0.98
    }
  ],
  "person_count": 1
}
```

## 语音活动检测（VAD）

输入固定为 RIFF/WAVE 容器中的单声道 16 kHz、16 位线性 PCM；请求仅要求 `input.data_b64`。格式提示若出现，必须符合该格式；adapter 默认文件名 `audio.wav`、媒体类型 `audio/wav`、16000 Hz、单声道。`threshold`、`stream` 可选。片段按顺序排列且不重叠，起点包含、终点不包含。`speech` 当且仅当片段非空时为 true；`probability` 是最高片段置信度，无语音时为 0。

```json
{
  "input": {
    "data_b64": "<base64-encoded PCM WAV audio>"
  }
}
```

```json
{
  "speech": true,
  "probability": 0.98,
  "segments": [
    {
      "start_s": 0.12,
      "end_s": 1.72,
      "confidence": 0.98
    }
  ]
}
```

## 语音合成（TTS）

TTS 将文本转换为 WAV 音频。仅 `text` 必需；`language`、`voice`、`instruction`、`temperature`、`top_p`、`reference_audio_b64`、`reference_audio_media_type`、`prompt_text` 和 `stream` 可选。Provider 专用字段名只在外部 adapter 中使用。

```json
{
  "text": "The update is complete."
}
```

```json
{
  "data_b64": "<base64-encoded WAV audio>",
  "media_type": "audio/wav",
  "format": "wav"
}
```

## 人脸识别

Northbound 和技能的类型化 `vision.face` 推理只允许 recognize。v1 schema 仍能表示旧 provider 操作，但通用类型化推理拒绝非识别调用；注册使用独立内部 EOS 操作。输入仅要求 `input.data_b64`，文件名和媒体类型为可选传输提示。

### 识别人脸

`recognize` 返回所有检测到的人脸；`return_image` 可请求标注后的输入图像。`direction_px` 是脸中心相对图像中心的水平位移，负值表示在左侧。`direction_px`、`annotated_image`、`message` 可选。只有内部 `Runtime::RegisterFace` 用于 `face.register` primitive 的 ArcFace 注册流程；类型化推理调用者不能选择注册操作。

```json
{
  "operation": "recognize",
  "input": {
    "data_b64": "<base64-encoded JPEG image>"
  },
  "return_image": true
}
```

```json
{
  "status": "success",
  "operation": "recognize",
  "data": {
    "faces": [
      {
        "identity": "Person A",
        "known": true,
        "similarity": 0.97,
        "bbox": [
          10,
          20,
          110,
          160
        ],
        "direction_px": -4.0
      }
    ],
    "face_count": 1,
    "annotated_image": "<base64-encoded annotated image>"
  },
  "message": "Recognition completed."
}
```

### ArcFace 请求时序

相机 runtime 等待人体检测完成，只在有跟踪人体时发送识别帧。身份未确定时以 250 ms 间隔调度；匹配已登记身份后至少间隔 1 秒。更长的配置最小间隔仍生效。worker 仅保留最新等待帧，在日志中记录排队、推理和帧龄，并丢弃源帧超过 3 秒的结果。

## 通用 VLM 请求

`vla-gemma4` 通过一个配置的服务和健康探测提供私有 `vision.vlm` 及定向姿态分类。通用请求接受文本对话与可选 JPEG；纯文本调用不需要相机。图像会由 `gemma4-unified-v1` adapter 附加至最新 user 消息，因此图像调用必须含 user 消息。调用方显式提供画面，模型 runtime 不自行采集。`input`、`temperature`、`max_tokens`、`reasoning`、`stream` 可选。`reasoning` 默认为 false，定向姿态分类始终关闭思考。响应为有界的 `{ "text": "..." }`，双方由私有 v1 schema 校验。

```json
{
  "messages": [{"role": "user", "content": "What is in front of me?"}],
  "input": {"data_b64": "<base64-encoded JPEG image>", "media_type": "image/jpeg"},
  "max_tokens": 128,
  "stream": false
}
```
