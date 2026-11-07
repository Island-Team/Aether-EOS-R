# EOS world interface

[中文](../../../docs_cn/ei/southbound/world.md) · [Southbound overview](../southbound.md)

EOS advertises only currently readable semantic resources. EGIA recognizes three resource families:

| URI | Contract | EGIA use |
|---|---|---|
| `robots/{robot}/world/state` | `world.snapshot@1` | fused entities and relations |
| `robots/{robot}/detection/state` | `detection.snapshot@1` | one model result with typed `human:*` and `obj:*` identities |
| `robots/{robot}/world/asr/state` | `asr.snapshot@1` | recognized external speech |

`WatchResource` reports only a changed URI. EGIA opens the watch before its initial read, validates every complete reread, coalesces identical snapshots, and reconnects after bounded rediscovery when a runtime-scoped route disappears. Missing resources remain unavailable; EGIA never fabricates empty perception.

World and detection are distinct. World state may fuse observations over time, while detection retains the producing device/model source and `source_sequence`. Detection attributes are schema-bounded semantic facts; raw tensors, masks, boxes, landmarks, embeddings, audio, and provider payloads remain inside EOS.

Camera images are not watched resources. `CaptureFrames` returns one `vision.snapshot@2` containing one or two ordered JPEG frames with dimensions and RFC3339 `capturedAt` timestamps. EGIA validates the envelope, requested count, JPEG structure, dimensions, timestamps, and distinctness before use. Raw microphone audio never crosses the interface.
