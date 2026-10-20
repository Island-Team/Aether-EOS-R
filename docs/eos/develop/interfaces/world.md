# World interface

[中文](../../../../docs_cn/eos/develop/interfaces/world_cn.md)

[Interface overview](README.md) · [Body discovery](body.md) · [Skills](skills.md) · [Workflows](workflow.md) · [Models](external/models.md)

World describes dynamic physical facts outside the robot. Robot devices, primitive bindings, health, actuator state, and power belong only to [body](body.md). Compact model observations are world products and are documented here alongside fused state.

| URI | Type | Meaning |
| --- | --- | --- |
| `robots/{robot}/detection/state` | `detection.snapshot` | semantic observations from one model result |
| `robots/{robot}/world/vad/state` | `vad.snapshot` | voice activity for one processed microphone window |
| `robots/{robot}/world/asr/state` | `asr.snapshot` | recognized external speech |

`ReadResource` returns the latest complete snapshot. `WatchResource` reports a changed URI, after which the client reads it. Configured retained resources are listed before their first output; a cold read or watch starts the shared producer and waits for a validated product. The `world.snapshot` contract remains in the canonical schema set for a future fusion producer, but the current runtime does not advertise or route `robots/{robot}/world/state`.

### Complete resource requests

`GatewayService.ListResources` takes an empty request and returns the currently readable URI descriptors. A complete protobuf request is:

```protobuf
ReadResourceRequest { uri: "robots/demo-0/detection/state" }
```

`GatewayService.WatchResource` uses the same request over gRPC server streaming. Each notification contains only the changed URI, after which the client calls `ReadResource` for the new complete snapshot:

```protobuf
WatchResourceRequest { uri: "robots/demo-0/detection/state" }
```

## External world state

The following is the retained canonical contract shape. It has no current runtime producer or readable resource URI.

```json
{
  "type": "world.snapshot",
  "schema_version": "1.0",
  "sequence": 26,
  "timestamp": "2026-09-20T09:30:00Z",
  "frame_id": "map",
  "entities": [
    {
      "id": "obj:part-7",
      "label": "bearing",
      "position_m": [2.0, 0.3, 0.08],
      "velocity_m_s": [0.0, 0.0, 0.0],
      "state": "stationary"
    },
    {
      "id": "human:7",
      "label": "person",
      "position_m": [3.1, -0.8, 0.0],
      "velocity_m_s": [0.2, 0.0, 0.0],
      "state": "walking"
    }
  ],
  "relations": [
    {"subject": "human:7", "relation": "near", "object": "obj:part-7"}
  ]
}
```
World entities are fused, tracked physical estimates—not image boxes or model keypoints. IDs use the same typed namespace as detection, allowing EOS to associate an observation with a world entity. Unavailable optional facts are omitted rather than emitted as large empty structures.

## Semantic detection

Detection is part of the world interface but remains a distinct resource type: a detection is evidence from one model run, while `world.snapshot` may fuse many observations over time. `robots/{robot}/detection/state` returns one complete `detection.snapshot`. IDs are typed and stable for the lifetime of a track: `human:<track>` for people and `obj:<track-or-domain-id>` for other objects.

```json
{
  "type": "detection.snapshot",
  "schema_version": "1.0",
  "sequence": 42,
  "timestamp": "2026-09-20T09:30:01Z",
  "source": "cam-0",
  "source_sequence": 317,
  "source_generation": 317,
  "source_frame_id": "camera_optical",
  "source_timestamp_ns": 1789983000000000000,
  "depth_available": true,
  "face_recognition_status": "complete",
  "detections": [
    {
      "id": "human:7",
      "kind": "human",
      "label": "person",
      "confidence": 0.94,
      "distance_m": 2.1,
      "bearing_rad": -0.12,
      "attributes": {
        "name": "Person A",
        "pose": "stand",
        "action": "waving",
        "activity": "waving",
        "speaking": false
      }
    },
    {
      "id": "obj:part-7",
      "kind": "object",
      "label": "bearing",
      "confidence": 0.96,
      "distance_m": 1.34,
      "bearing_rad": 0.18,
      "attributes": {
        "pickable": true
      }
    }
  ]
}
```

`sequence` identifies the on-demand resource envelope and may advance on each read. `source_sequence` identifies each captured camera frame and advances before inference completes. `models.human`, `models.pose`, `models.action`, and `models.face` independently report `status`, model-result `sequence`, the `source_generation` that produced the retained result, and the newest `requested_generation`. Completed values and semantic attributes remain visible while slower work is pending. Configured stages without a usable result report `unavailable` and use explicit placeholders such as `pose = "unknown"`, `action = "unknown"`, and `name = null`; only absent model configuration reports `not_configured`. A single source generation can therefore produce several watch/history updates. Skills that require completed inference must check the relevant model status and sequence rather than treating the top-level source sequence as model completion. A newer inference result alone does not prove that the camera captured its image after the motion; that requires capture and motion timestamps in a comparable clock domain.

When a configured gimbal is mounted on the same robot, `source_gimbal` carries the pan, tilt, axis rates, and EOS monotonic sampling time retained with the camera frame. Delayed detections therefore remain associated with the mount observation sampled for their source frame.

`source_generation` identifies the latest captured RGB sample. Source, human, pose/action, and face updates are published independently. When an asynchronous face observation is available, `face_observation` reports its own `source_generation` and its `age_ms` when the current detection snapshot was assembled. EOS first matches faces to people in that source generation, then carries each identity onto newer pose results with the same track ID. A face update does not advance `source_sequence`, because it is not a new pose inference. Cached observations expire after three seconds so a recycled track ID cannot retain an old name. `source_frame_id` is the camera optical coordinate frame, not a video-frame number. `source_timestamp_ns` preserves the camera's original timestamp and is omitted when unavailable; EOS does not substitute a timestamp from another clock. The envelope `timestamp` is the time the resource was read, not the camera capture time.

`depth_available` says whether aligned depth was retained for that RGB sample. Raw depth images are not published in the detection snapshot. `face_recognition_status` is `pending`, `complete`, `unavailable`, or `not_configured`. `complete` means a cached face observation is available; its freshness is explicit in `face_observation`. The same `source_generation` is local to a camera-pump lifetime; consumers should also compare `source` and use `source_sequence` for fresh-result checks.

Camera object detection is exposed separately at `robots/{robot_name}/cameras/{camera_device_id}/detections/state`. It uses the same snapshot contract, carries the selected camera ID as `source`, its own source sequence and generation, and only object detections. The legacy `robots/{robot_name}/detection/head/state` URI aliases the configured default object camera. These resources remain separate from the human pose detection resource, so object frames cannot satisfy pose-frame freshness checks.

Required fields are `id`, `kind`, `label`, and `confidence`. Distance, bearing, and semantic attributes are included only when known. Camera-model snapshots also expose `image_width_px`, `image_height_px`, and each detection's optional `bbox` as left, top, right, and bottom source-image pixels. These bounded values support reviewed visual-servo skills. Pose-enriched humans optionally expose 17 COCO `keypoints`, their `keypoint_scores`, and aligned `keypoint_depth_m`; an invalid or low-confidence joint has null depth. The top-level `pose_observation` identifies the pose source and age because joints may be retained onto newer bbox frames by track ID. Masks, embeddings, and depth maps remain excluded.

EOS publishes human boxes to `robots/{robot}/detection/state` only when their finite detection confidence is at least `0.4`; this does not change the model's internal tracking or the separate face-similarity threshold.

For human detections, `attributes.name` contains the recognized identity. A known face accepted at similarity `0.40` or greater contains its registered name. EOS retains that name across all empty, unrecognized, or lower-similarity face results while the same explicit unique track remains present, regardless of bbox motion. An unrecognized face without a retained identity contains `"unknown"`; a tracked person with no safe matched identity contains `null`. A different track, duplicate ID, or disappearance clears the retained identity. When an identity is present, `attributes.name_source_generation` identifies the camera generation whose accepted face observation established the retained name, and `attributes.name_similarity` contains that observation's score. These fields remain unchanged while later human frames reuse the cached identity.

For human detections, `attributes.pose` contains YOLO's `stand`, `sit`, `lie`, or `unknown`. `attributes.action` contains the fused `waving`, `fallen`, `rest`, or `unknown`. EOS maps legacy `none` and invalid actions to `unknown`. `pose_confidence` and `action_confidence`, when supplied for the published value, are bounded to `[0, 1]`; confidence for a normalized invalid value is omitted. Furniture posture remains internal to the model pipeline and is not published as `attributes.posture_state`. Provider debug fields remain excluded.

`attributes.activity` is temporary compatibility output. It contains `waving`, `fallen`, or `rest` when that action is meaningful; otherwise it contains the pose. Consumers should migrate to `pose` and `action`. EOS does not infer walking from a standing posture.

## Vision snapshot

A camera northbound operation returns pictures, never a video or stream. `GatewayService.CaptureFrames` requests one or two recent raw RGB frames and returns one `vision.snapshot`. Vision is on demand, not a readable/watchable resource, so EOS does not repeatedly publish image bytes:

```json
{"robotName": "demo-0", "deviceId": "cam-0", "frameCount": 2}
```

```json
{
  "type": "vision.snapshot",
  "schema_version": "1.0",
  "deviceId": "cam-0",
  "frames": [
    {
      "jpeg": "/9j/4AAQSkZJRg...",
      "widthPx": 1280,
      "heightPx": 720,
      "capturedAt": "2026-09-20T09:30:01.000Z"
    },
    {
      "jpeg": "/9j/4AAQSkZJRg...",
      "widthPx": 1280,
      "heightPx": 720,
      "capturedAt": "2026-09-20T09:30:01.033Z"
    }
  ]
}
```

Frames are distinct cached JPEG pictures in capture order. EOS rejects unknown or unhealthy cameras, invalid counts, stale or malformed frames, and oversized payloads. There is no northbound camera stream and no microphone equivalent.

Face enrollment is a camera action, not a world resource operation. Use the [`face.register` primitive](actions.md#face-registration-advanced).

Registration follows the primitive acknowledgement contract: schema-invalid requests fail before dispatch, while camera, model, and provider refusals return a complete acknowledgement with `accepted: false` and a stable `reason_code`. EOS does not define how a provider selects among multiple faces or handles an already registered name; those policies and identity persistence remain provider-owned.

## Voice activity

VAD is a bounded retained resource. Every successful VAD model result appends one product, including the result's source sequence and the microphone source timestamp when supplied. It deliberately excludes PCM, WAV, utterance audio, segments, transcripts, and model/provider identifiers.

```json
{
  "type": "vad.snapshot",
  "schema_version": "1.0",
  "sequence": 18,
  "timestamp": "2026-09-20T09:30:01Z",
  "source_sequence": 317,
  "source_timestamp_ns": 1789983000000000000,
  "speech": true,
  "probability": 0.91
}
```

The runtime retains the latest 128 products per process. `ListResources`, `ReadResource`, `WatchResource`, and `QueryResourceHistory` expose the declared URI through the common resource APIs. Reads and watches acquire demand on the shared `microphone_vad` producer; history queries inspect products already retained.

## Recognized speech

ASR exposes only the semantic result needed by an agent. Raw PCM, token timing, and acoustic features remain internal to EOS; VAD state is available only via its separate compact resource.

```json
{
  "type": "asr.snapshot",
  "schema_version": "1.0",
  "sequence": 31,
  "timestamp": "2026-09-20T09:30:02Z",
  "text": "Move to the loading bay.",
  "language": "en",
  "confidence": 0.97
}
```
