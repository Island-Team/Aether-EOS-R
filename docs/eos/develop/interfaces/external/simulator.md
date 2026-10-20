# Standalone simulator interface

[Chinese](../../../../../docs_cn/eos/develop/interfaces/external/simulator_cn.md)

`eos-simulator` is an explicitly launched process. It owns the demo world, monotonic clock, maps, ray casting, source decoding, persistence, and browser API. `eosd` neither starts nor supervises it and contains no simulator world state. Stopping or restarting the simulator must not terminate EOS.

## Process boundaries

| Interface | Default | Consumer |
| --- | --- | --- |
| private gRPC over Unix socket | `<run>/simulator.sock` | demo adapters in `eosd` |
| HTTP and WebSocket | `127.0.0.1:16004` | WebUI |

The private `eos.simulator.v1.SimulatorService` provides bounded `Health`, `Execute`, `ReadTelemetry`, and `ReadCameraFrame` calls. The socket is owner-only and calls have finite deadlines. When it is absent or disconnected, demo devices report unavailable, commands reject, and the camera returns no frame. Later calls reconnect; other EOS services continue normally.

The browser cannot submit device state, camera pixels, detections, model responses, held state, or primitive results. Those remain simulator/EOS outputs.

HTTP and WebSocket connections share a hard limit of 64 session workers. Request parsing runs in those workers rather than the accept loop, completed workers are reaped before their slots are reused, and the listener leaves new connections in the kernel backlog while all slots are occupied. Shutdown closes every accepted transport before joining the workers, interrupting pending reads and writes.

## Configuration and lifecycle

`etc/eos/sim.toml` is strict-keyed:

```toml
[simulator]
host = "127.0.0.1"
port = 16004
map_id = "lab"
camera_source = "system"

[simulator.camera]
width_px = 1920
height_px = 1080
fps = 30
```

`host` must be loopback and the configuration file uses the reserved default port `16004`. Development tooling may pass `eos-simulator --port N`; `eos-launch.sh` does this automatically when another local process occupies the default. `map_id` selects a stable map below the simulator state root. An empty ID starts with an in-memory white fallback. `camera_source` is either `system` or `scene`. `system` opens the operating system's default camera through GStreamer's `autovideosrc` at the exact width, height, and frame rate required by `[simulator.camera]`, and makes it the simulator's only camera source; source-line hits and selected source IDs are suppressed. Camera dimensions are bounded to 1..16384 and `fps` to 1..240; unknown or missing camera keys are rejected. If the camera is absent or interrupted, capture retries and the demo camera returns no frame until a new matching JPEG arrives. A source that cannot negotiate the configured mode stays unhealthy rather than silently falling back. `scene` retains the ray-selected source-library behavior below. Changing the active map or camera source requires restarting `eos-simulator`; committing that map's manifest or raster hot-reloads it atomically.

## V2 persistence

The clean-break layout is rooted at `<state>/simulator/`:

```text
simulator/
├── maps/<map-id>/manifest.json
├── maps/<map-id>/raster.png
├── library/index.json
└── library/files/<managed-file>
```

Legacy `<state>/demo/maps/*/assets` bundles are neither read, migrated, nor deleted. Paths are service-generated canonical paths; clients cannot store arbitrary host paths.

A map manifest has `version = 2`, a stable `id`, unsigned `revision`, positive raster dimensions and resolution, an origin, and `lines`. Each line has a stable `id`, global `source_id`, planar `center`, finite `yaw_rad`, and positive `length_m`. `source_id` must name an entry in `library/index.json`; media metadata and paths are not embedded in map manifests. IDs follow the bounded store grammar and are never paths.

The global source index has its own revision and stores stable source IDs, admitted media types, managed filenames, and byte sizes. Deletion is rejected while any map references the source. Manifest, raster, source, and index updates validate before an atomic fsync-and-rename commit; failures leave the prior revision readable.

## Browser API

All routes are under `/v1/eos/simulator`; removed `/v1/eos/demo/*` routes have no aliases. Requests and responses are bounded. Existing-resource mutations require the current revision in `If-Match`; stale revisions return `409`, a missing precondition `428`, malformed input `400`, and missing resources `404`. Revisioned responses expose an `ETag` where applicable.

| Method | Route | Purpose |
| --- | --- | --- |
| `GET` | `/` | API version and active map ID |
| `GET` | `/snapshot` | atomic live simulator snapshot |
| `GET` | `/raster` | active map's canonical PNG |
| `GET`, `POST` | `/maps` | list or create maps |
| `GET`, `PUT`, `DELETE` | `/maps/{id}` | read, replace, or delete a manifest |
| `GET`, `PUT` | `/maps/{id}/raster` | read or replace the PNG |
| `GET` | `/sources` | global source-library index |
| `GET`, `PUT`, `DELETE` | `/sources/{id}` | download, upload/replace, or delete media |

The service admits supported image/video media types and enforces byte, image, and decoded-frame bounds. It validates map/source references, dimensions, PNG encoding, canonical paths, and revisions before commit. CORS is limited to the documented production origin and local WebUI development origins.

### WebSocket

`GET /v1/eos/simulator/ws` upgrades to the authoritative live channel. Every JSON envelope contains `version: 2` and `type`; requests may carry `request_id`, copied into their response. A connection receives a `snapshot`; clients send `resync` after a reconnect or revision gap. Unsupported versions/types and malformed or oversized messages receive bounded structured `error` envelopes and never mutate state.

Snapshots carry the active map/revision, device state, and selected line/source when a vision ray hits. Map/source commits publish revision-bearing events; active-map commits publish a fresh snapshot after hot reload. On a gap, clients replace local state from a snapshot rather than replaying changes.

## Vision-source resolution

When `camera_source = "scene"`, the simulator casts a horizontal ray from the base at `base.yaw + gimbal.pan`, intersects finite source lines, and selects the nearest positive hit. No hit records empty selected IDs and yields no demo camera frame.

On a hit, the simulator resolves `line.source_id` through the global library and opens only that canonical managed file. Images/videos are decoded lazily; video frames are produced only while selected. A bounded LRU cache holds decoded sources. Source lines are camera triggers, not collision geometry or semantic detections. EOS still routes the camera frame through its configured model and validation pipeline.

When `camera_source = "system"`, a restartable capture worker continuously converts the system-default camera to bounded JPEG frames and retains only the newest frame. Map source lines and library media do not participate in camera output in this mode.

## Logging and failures

Structured logs cover startup/shutdown, IPC connection/recovery, WebSocket session counts, map/source commits, hot reloads, source-selection changes, decode/cache failures, revision conflicts, and bounded shutdown counters. Lifecycle/commits are `info`, state transitions `debug`, recoverable failures `warn`, unrecoverable service/storage faults `error`, and opt-in timings `trace`.

Ticks and successful frames are not logged. Logs never include media bytes, raster pixels, full manifests, command payloads, credentials, or WebSocket bodies. Repeated disconnect/decode warnings are deduplicated or rate-limited.
