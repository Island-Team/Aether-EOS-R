# `egia.core.perception`

[中文](../../../docs_cn/ei/core/perception.md) · [Core overview](modules.md)

Perception is the only admission point for external observations. Records carry kind, source alias, producer and admission time, source sequence, text, and typed metadata. Per-kind history is bounded and replaceable state coalesces by source.

## Implementation map

`admission.py` owns revisioned queue/history state and change notifications. `distribution.py` exposes narrow context, safety, monitor and optional end-to-end ports. `body/source.py` reads and watches EOS resources; `body/__init__.py` validates typed snapshots and publishes parts/scenes; `body/render.py` renders one detection. The empty `world` package reserves ownership only and has no runtime producer.

EOS resource sources decode body parts, robot status, scene objects, vision detections, hearing, faults, and primitive results. A distribution layer exposes typed consumers to context, safety, monitor, and I/O without giving those modules ownership of the ingest queue.

Current body-part snapshots are also admitted as compact replaceable `body` status lines per robot and device. Each line carries an explicit `health=ok | alert | disabled` verdict and a bounded semantic projection. Actionable categorical pose is retained—for example `posture=lie` tells the model to stand before navigation—along with device availability, held-object identity, faults, and emergency state. Binary camera data and numeric coordinates, velocities, orientations, joint arrays, forces, and similar measurements without model-facing frames or thresholds are omitted; typed snapshots and safety calculations retain full source precision.

The first replaceable snapshot from each `(kind, source)` is logged at INFO. Later coalesced replacements remain available in the DEBUG replay stream but do not flood the human perception log; queue admission and bounded history are unchanged.

The newest hearing observation is promoted out of the bulk body/detection/object summary into its own higher-priority context section. High-rate body updates therefore cannot crowd directed speech out of the model budget, while the untrusted-hearing label preserves model judgment for noise and speaker echo.

Context renders compact inline object and detection summaries. Collections above the bound report an exact omitted count; perception does not create retrieval handles. Source identity is preserved across every multi-backend projection.

Raster-map types remain checked contracts only. Perception has no map source, poller, frame exchange, storage, admission kind, or northbound producer in the MVP.

## Data lifecycle and conditions

An admitted envelope is validated before it can mutate state. The admission queue bounds work in flight; bounded per-kind history retains ordered events, while replaceable observations keep only the newest value for each `(kind, source)` identity. Distribution reads these projections without taking queue ownership. Draining marks every dequeued item complete, and cancellation stops consumers without inventing observations.

Empty history and a source that has not published yet produce an absent projection, not a fabricated default. A new replaceable source is rejected when admitting its first identity would exceed capacity; later values for an admitted identity replace its current value. Malformed envelopes, invalid typed payloads, and unsupported kinds are rejected before history or replaceable state changes. Oversized object or detection collections remain valid and are summarized with an exact omitted count only at the context boundary.

EOS resource streams retry after transport and decode failures. A disconnect leaves the last typed snapshot available but does not relabel it as fresh; a later valid sample replaces it. Coalescing duplicate high-rate state does not alter event observations such as hearing, faults, or primitive results. On shutdown, source tasks observe the stop signal and blocked waits are cancelled; no final observation is fabricated, and process exit discards any in-memory queue entries that consumers did not drain.

Body and world snapshots preserve source precision and attribution. Semantic status projection, omission of numeric telemetry and binary image bodies, and promotion of current hearing are model-facing transformations only. Missing devices, disabled cameras or microphones, empty scenes, and non-pickable detections remain explicit states rather than disappearing through truth-value filtering.
