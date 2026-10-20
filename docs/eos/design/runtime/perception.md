# Perception runtime

[中文](../../../../docs_cn/eos/design/runtime/perception_cn.md)

[Runtime overview](README.md) · [Body interface](../../develop/interfaces/body.md) · [World and detection interface](../../develop/interfaces/world.md) · [Internal model interfaces](../../develop/interfaces/external/models.md)

`src/eos/runtime/perception/` is the namespace and ownership boundary for read-only semantic projection. It turns admitted hardware observations and normalized model results into canonical body and semantic resources. It does not own hardware backends, call provider-specific model APIs, execute skills, or release actuator commands.

## Folder structure

```text
src/eos/runtime/perception/
├── product_history.*       bounded validated retained products
├── producer_coordinator.* producer demand and lazy startup
├── resource_catalog.*      declarations, routing, and history lookup
└── body/                   canonical body, detection, vision, and speech projection
```

The top-level folder builds `eos_perception_history`, `eos_perception_producers`, and `eos_resource_catalog`. The catalog combines the retained product path with the `eos_body` reader used for cheap device state. Northbound consumes that combined boundary rather than owning producer lifecycle or retained model state itself.

## Dependencies

Perception reads immutable device identity, configuration, health, and feed snapshots from the [hardware runtime](hardware.md). Camera-pose, VAD, and ASR snapshots arrive through the canonical [models runtime](models.md). Each source remains independently available: a missing model result does not erase valid hardware inventory, and an unavailable device does not create a synthetic observation.

The nested Reader supplies canonical payloads to the [northbound runtime](northbound.md). Resource shapes are defined by the northbound [body](../../develop/interfaces/body.md) and [world](../../develop/interfaces/world.md) interfaces.

## Projection

Body discovery projects compact admitted device identity, health, state, primitive bindings, and a `state_uri` into `Discover`. Every supported admitted device is also exposed directly at `robots/{robot_name}/devices/{device_id}/state`; these cheap reads require no model producer. Model-backed projection owns these independently available resources:

- `robots/{robot_name}/detection/state`
- `robots/{robot_name}/world/vad/state`
- `robots/{robot_name}/world/asr/state`

Configured retained resources are declared and listed before their first product. A cold read or watch acquires producer demand and waits for a new product; configured continuous streams hold the same producer lease. Detection contains compact `human:*` and `obj:*` semantic observations without masks, keypoints, or model tensors. Camera feeds remain outside readable resources; northbound owns the bounded two-picture cache used by `CaptureFrames`. VAD publishes only speech, probability, and source sequence/timestamp, while ASR projects only text, language, and confidence. Microphone PCM, WAV, utterance audio, model segments, and provider metadata stay inside EOS. The canonical `world.snapshot` schema remains reserved for a future real fusion producer; the body reader neither advertises nor routes it.

Published history accepts only JSON objects that pass the immutable validator associated with the declaration's canonical schema ID. History is bounded and process-local, but its values and correlation metadata (`boot_id`, `epoch`, sequence, and observation time) can still be sensitive. They are exposed through the same northbound resource boundary as the latest value; deployment policy is responsible for access control, and this component does not add a separate authorization layer.

Per-device files project common health and operational state. Gimbal device state also includes measured pan/tilt position and angular velocity so tracking processes can observe physical motion. `inventory.cpp` lists available resources, `router.cpp` resolves canonical URIs, and `semantic.cpp` projects compact VAD and ASR model output. Envelopes use monotonic sequence values and timestamps.

## Boundaries

- Perception is read-only. It cannot obtain mutable backend handles or call dispatch.
- Provider field names stop at `ext/models`; perception sees only canonical model responses.
- Raw microphone PCM stays inside EOS. Camera frames are not readable state resources; northbound owns the separate bounded `CaptureFrames` cache.
- Partial data remains partial. Missing devices, stale feeds, or absent model results are omitted or reported unavailable according to the resource contract.

## Verification

Tests under `src/eos/runtime/perception/tests/` cover history bounds and selectors, schema admission, producer lease transitions, lazy start/stop, and catalog routing. Tests under `src/eos/runtime/perception/body/tests/` cover every device class, partial inventories, device ownership, and semantic projection.
