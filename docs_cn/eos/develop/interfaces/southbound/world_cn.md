# 硬件世界输入接口

[English](../../../../../docs/eos/develop/interfaces/southbound/world.md)

[接口总览](../README_cn.md) · [硬件本体](body_cn.md) · [驱动原语](primitives_cn.md)

该接口向 EOS 提供原始或最低限度归一化的传感器观测。EOS 再产生北向可读的语义快照；音频和 VAD 推理留在内部。下列 JSON 仅说明 C++ 数据形状。身份和执行器状态见本体接口。

## 相机帧

`frame_id` 指相机坐标系，通常是光学坐标系；它不是帧编号，也不能固定为 `map`。来源时间戳可缺省，EOS 仍可根据尺寸与编码内容去重。每台相机最多缓存两个不同的有界帧。

```json
{
  "width_px": 1280,
  "height_px": 720,
  "frame_id": "camera_optical",
  "source_timestamp_ns": 1789983000000000000,
  "jpeg_b64": "/9j/4AAQSkZJRg..."
}
```

## 麦克风窗口

`frame_id` 指设备或阵列安装坐标系。时间戳可缺省；PCM 只在 EOS 内部进入 VAD 或 ASR，不穿过北向接口。无有效采样时返回缺失值，不伪造空样本。

```json
{
  "sample_rate_hz": 16000,
  "channels": 1,
  "frame_id": "mic",
  "source_timestamp_ns": 1789983000000000000,
  "pcm_s16le_b64": "AAACAAMABAAFAAYA..."
}
```
