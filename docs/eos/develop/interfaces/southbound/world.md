# Hardware world interface

[简体中文](../../../../../docs_cn/eos/develop/interfaces/southbound/world_cn.md)

[Interface overview](../README.md) · [Hardware body](body.md) · [Driver primitives](primitives.md)

The southbound world facet supplies raw or minimally normalized sensor observations to EOS. It may run at high or low frequency. EOS, not the driver, turns these values into on-demand RGB frames, compact semantic detections, external world state, VAD state, or ASR snapshots. VAD inference and all audio remain internal; only the bounded semantic `vad.snapshot` crosses northbound. Hardware identity, health, and actuator state are all documented in the [body facet](body.md). The JSON below illustrates native C++ structures; it is not a serialized HAL contract.

Transforms derived from the robot description, objects, detections, labels, confidence, pickability, and held-object association do not belong to this interface. EOS produces applicable detection values through its internal processing and model interfaces, then publishes them [northbound](../world.md). The canonical external-world schema has no current fusion producer or readable route.

## Camera frame

```json
{
  "width_px": 1280,
  "height_px": 720,
  "frame_id": "camera_optical",
  "source_timestamp_ns": 1789983000000000000,
  "jpeg_b64": "/9j/4AAQSkZJRg..."
}
```

`frame_id` names the camera coordinate frame (normally the optical frame from the robot description). It lets EOS relate pixels or derived rays to the rest of the robot. It is not a media-frame number and must not be hard-coded to `map`.

`source_timestamp_ns` is optional when the source exposes no clock. When absent, EOS still accepts the frame and deduplicates it from its dimensions and encoded bytes.

EOS validates and bounds frames before adding at most two distinct observations per camera to the northbound capture cache.

## Microphone window

```json
{
  "sample_rate_hz": 16000,
  "channels": 1,
  "frame_id": "mic",
  "source_timestamp_ns": 1789983000000000000,
  "pcm_s16le_b64": "AAACAAMABAAFAAYA..."
}
```

For a microphone, `frame_id` identifies the microphone or microphone-array mounting frame so spatial-audio processing can locate it. Non-spatial audio may still use the configured device frame for provenance.

`source_timestamp_ns` is optional when the capture source exposes no timestamp.

PCM remains inside EOS and may feed VAD or ASR; it never crosses northbound.

Camera JPEG and microphone PCM are world inputs. An absent read is represented as no sample, not a fabricated zero-valued observation. Speaker playback is output-device status and therefore belongs to the [body facet](body.md), not this world facet.
