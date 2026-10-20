# Configuration

`etc/` is the reference config root: packaged defaults and their schemas, sitting outside any installed target. A deployment points `EOS_ETC` (or `--etc`) at its own root; if neither resolves, the server refuses to start and names the resolved path in the error. No compiled-in fallback endpoint.

`[gateway]` contains `host` and `grpc_port`, defaulting to `127.0.0.1:16003`; port `16004` remains reserved for the external simulator. Optional `grpc_uds_path` adds a Unix-domain listener alongside TCP, and `grpc_uds_mode` sets its permissions (default `0o600`). The path must be absolute and normalized, its parent directories must already exist without symlinks, and its immediate parent must be process-owned with mode `0700`. This private parent prevents access while gRPC creates the socket and EOS applies `grpc_uds_mode`, without a process-global `umask` change. Startup probes an existing socket and removes it only when connection refusal demonstrates that it is stale; a live listener, non-socket entry, or inconclusive probe fails startup. Because the parent remains private, group/world socket mode bits do not grant access through that directory; deployments needing shared access must provide a separately secured transport boundary. `--host` and `--grpc-port` override the TCP address with the same validation. Both listeners serve `GatewayService`, `SkillService`, and `WorkflowService` over gRPC. EOS no longer accepts HTTP directly; HTTP clients use the separately launched adapter.

`[workflow] journal_enabled` optionally enables the authoritative ActionList journal. It defaults to `false` when omitted; the committed reference enables it. EOS derives the only storage location as `${state}/workflows/workflow.journal`, so configuration and clients cannot supply a path. The journal retains complete ActionList revisions, results, and accepted facts for restart recovery and must be treated as sensitive process state. Installed `share/workflows` files are read-only templates and are never used as the writable journal.

`etc/eos/north-http.toml` configures the independent client-facing process. Its strict `[north_http]` table owns HTTP `host`/`port`, the EOS gRPC `upstream`, the `allow_remote` safety opt-in, exact CORS origins, body/concurrency bounds, readiness and request deadlines, and SSE heartbeat interval. Command-line `--host`, `--port`, `--upstream`, and `--log-level` override the selected file. The default is loopback `127.0.0.1:16005` to upstream `127.0.0.1:16003`; `unix:/absolute/path.sock` selects an EOS Unix socket instead. A non-loopback HTTP bind is rejected unless `allow_remote = true`. The admitted upstream constructs one process-wide gRPC channel and one Gateway/Skill/Workflow stub set; it is not duplicated by route or request. See the [north HTTP configuration module](../develop/ext/north_http/config.md) and [complete adapter contract](../develop/interfaces/external/http.md).

The same file's strict `[action_list_audit]` table owns `enabled`, `rotation_size_bytes`, `rotation_files`, and `reconnect_delay_ms`. Missing the table preserves compatibility by disabling audit; defaults are `false`, 16 MiB, 8 files, and 1000 ms, while the committed reference explicitly sets `enabled = true`. Numeric maxima are 256 MiB, 64 files, and 60,000 ms, and the overflow-safe size/count product is capped at 256 MiB. The journal directory is derived as `${state}/north-http/action-list-audit`; no path key is admitted. Audit uses the process shared Workflow stub and remains external and non-authoritative.

`[log]` controls the synchronous logging sinks. `level`, `file_level`, and `jsonl_level` set the stderr, routed human-file, and JSONL thresholds. A supervised launch shares one directory beneath `var/log/eos/runs/<launch-id>/`, with `launcher.log` at its root; each service writes `events.jsonl`, `main.log`, `runtime-<module>.log`, `ext-<module>.log`, and `thirdparty.log` directly below `<service>/`. `var/log/eos/latest` points to the newest launch. With the default `overwrite = false`, each launch is retained under a unique ID; `overwrite = true` instead reuses and clears the entire `runs/current` launch, including logs and attachments, before writing new output. Retained runs remain untouched. `rotation_size_bytes` and `rotation_files` default to 64 MiB and five retained copies. `dedup` enables identical-event suppression in file sinks and `dedup_window_s` is the required quiet period before an identical event is saved again. Each suppressed occurrence refreshes this period. `save_attachments` persists selected binary payloads in the flat `var/log/eos/runs/<launch-id>/attachments/` directory, also reachable as `var/log/eos/latest/attachments/`. Files are category-prefixed, for example `asr-<uuid>.wav`, `tts-<uuid>.<ext>`, and `vision-<uuid>.jpg`; no category subdirectories are created. Attachment events include `category`, `relative_path`, media type, and byte size; binary content is never embedded in text or JSONL logs. These settings do not control durable workflow or ActionList audit journals under `var/lib`.

`[[launch.prelaunch]]` starts deployment-owned work before EOS constructs its hardware adapters. Each entry contains exactly one of `command` or `shell`. `command` is a non-empty string array executed directly and synchronously; a non-zero exit status refuses EOS startup. `shell` is a non-empty Bash command for a long-running service: EOS starts it asynchronously, captures its combined stdout and stderr in the launch's `eosd/prelaunch-<index>.log`, and terminates its process group during shutdown. Entries run in declaration order. A shell entry may set `reuse_if_ready = true` with a non-empty `ready_shell`. EOS probes that command for up to two seconds before starting; when it succeeds, EOS reuses the existing service and does not stop it on shutdown. Set `restart_on_exit = true` for a foreground `shell` service that should be relaunched two seconds after it exits. If it also uses `reuse_if_ready`, the supervisor leaves an already-ready external process alone, checks its readiness every two seconds, and starts its own service only after the external one disappears. The restarted process remains EOS-owned and stops with EOS. This option detects process exit, not a live service that has become unresponsive. By default, REPL `exit` stops EOS and its prelaunch services so the next launch starts with free hardware ownership. Deployments may configure their own services and readiness checks.

Optional `[launch.adapter]` starts an EOS-owned long-running ROS adapter after prelaunch. It requires `shell` and `ready_shell`; EOS probes readiness until `timeout_ms` (default 30000, maximum 300000), fails startup if the adapter exits or never becomes ready, records output in `eosd/adapter.log`, and terminates its process group during shutdown. With `reuse_if_ready = true`, an already-ready adapter is reused before spawning another one and remains externally owned. `--no-prelaunch` disables both prelaunch and the managed adapter. The adapter commands and readiness probe belong to the deployment; use the matching sourced ROS 2 environment and avoid competing ownership of hardware devices.

`[[perception.model]]` configures an outbound client for a separately deployed model service. Each entry requires a process-unique logical `id`, supported `capability`, and absolute numeric-host endpoint. `transport` defaults to `http`; `websocket` requires a `ws://` endpoint, JSON wire format, and a vision capability. HTTP endpoints use `http://` and may select `json` or `multipart`; `http_method`, `file_field`, string-only `fields`, and `api_key_env` are HTTP-only. `adapter` selects a provider mapping implemented under `src/eos/ext/models`; it defaults to `canonical`. `timeout_ms` defaults to 3000 and is bounded to 300000. `max_request_bytes` and `max_response_bytes` default to 16 MiB. `provider_model_id` is available to the selected adapter and is never inserted into a canonical caller request by the core. `health_endpoint` remains an optional `http://` probe; omitting it leaves normalized health unavailable and logs one configuration warning.

`[perception.stream]` strictly admits only `vad`, `human`, `human_pose`, and `face`. Each switch is boolean and defaults to `false`. The committed configs select `vad = true`, `human = true`, `human_pose = true`, and `face = false`. The human stream supplies tracked boxes and the human-pose stream enriches the same source frames with canonical pose and action semantics.

Optional `enabled` defaults to `true` and is read only at startup. Disabled entries remain fully validated and participate in process-wide model-ID deduplication, but are absent from discovery and calls to their exact IDs return `NOT_FOUND`. EOS does not substitute a built-in model or select another model by capability.

Canonical ASR, TTS, and VAD requests accept `stream = false` by default. The buffered call API rejects `stream = true` explicitly. Provider aliases and payload reshaping belong to the adapter rather than this configuration contract.

The committed reference configuration declares eight logical clients. Human and human-pose use persistent WebSocket services, ArcFace registration uses a separate WebSocket client, and ArcFace recognition, Audio8, multipart ASR, VAD, and health probes use HTTP. Provider mappings remain selected with `adapter`, and the runtime contains no built-in inference fallback. `etc/eos/test/eos.test.toml` uses loopback endpoints for configuration tests.

Native skills are deployment-owned and selected outside the perception interfaces; see [Skill authoring](../develop/interfaces/skills/native/README.md). Source-checkout launches default to the selected build tree's `skills/` directory when it exists. Installed wrappers similarly select `<libdir>/island/skills` when the package installed shared skills there. Explicit `--skills-dir PATH` or `EOS_SKILLS` selects another absolute directory. EOS loads regular, non-symlink `*.skill.so` libraries before opening the gateway; libraries are neither uploaded, watched, modified, nor persisted by EOS.

## Layout

```
etc/
├── eos.example.toml    # committed template; copy to the ignored local eos.toml
├── eos.toml            # deployment-local sections: [gateway] and [log],
│                        #   [[perception.model]] outbound endpoints, log levels,
│                        #   overwrite, repeat suppression, and attachments, plus one
│                        #   [[mode.binding]] selecting this process's hardware
│                        #   plus [[launch.prelaunch]] startup commands
├── north-http.toml   # standalone REST/SSE listener, upstream, CORS, and limits
├── sim.toml            # standalone eos-simulator; map_id and fixed loopback port 16004
├── backends/            # backend bindings (relative to this root)
│   ├── demo.toml        #   public demo and demo-host profiles
│   └── <site>.toml      #   deployment-owned hardware binding
└── safety.toml          # future Dispatcher policy: limits, estop, gate deadline
```

## Binding files

Every binding file is a **profiled whole-robot description**: one `[profile.<name>]` table per environment, with robot identity in `[profile.<name>.robot]` and devices in `[[profile.<name>.device]]`. `eos.toml` selects exactly one `(file, profile)` per EOS process; fleets aggregate multiple processes. `robot_id` is a stable configured binding key (a generated UUIDv7 is only a fallback), `robot_name` is the fleet-unique human route name, and `robot_desc` describes that concrete robot. Device IDs remain deterministic `<class>-<zero-based-index>` strings. Legacy `id` keys fail with a migration error. The profile carries descriptions, devices, command surfaces, admitted numeric ranges, feed rates, and transports. URDF and controllers remain the physical authority for kinematics, geometry, reach, payload, and hardware protection; the binding mirrors caller-visible numeric command intervals so registry discovery and dispatch use the correct device envelope. Each `[[device]]` splits what a device is from how EOS reaches it:

- `class` — the semantic device kind: `arm`, `base`, `gimbal`, `cam`, `mic`, `gripper`, or `speaker`. The class selects the northbound resource family and the part contract.
- `[device.backend]` — the forwarding adapter: `type = "demo"`, `"ros2"`, `"alsa"`, `"v4l2"`, or `"aplay"`. The registry discovers the `(type, class)` pair from this configuration and constructs only that adapter. Demo implements all device classes; ROS 2 implements `arm`, `gripper`, and `cam`; `alsa` implements `mic`; `v4l2` implements `cam`; `aplay` implements `speaker` and adds a `[device.backend.aplay]` table.

Device keys: `class` (required); `part_id` (default `"default"`), `description`, `tags`, and `end_effector` (free-form deployment metadata, e.g. `"none"`) optional. Do not configure `id`. The `[device.part]` table carries per-class static scalars, all optional with the defaults below; a key a class does not define is a parse error naming the file and key:

```text
base:    footprint_length_m (0.5), footprint_width_m (0.3)
gimbal:  fov_rad (1.4), range_m (5.0)
cam:     fov_rad (1.4 for demo; unknown for direct or ROS 2 hardware unless configured)
arm:     link_lengths_m ([0.18, 0.16, 0.12]), mount_x_m (0.0), mount_y_m (0.0), initial_joint_positions_rad (unset)
mic, gripper, speaker: none yet
```

`link_lengths_m` holds up to six non-negative link lengths in link order. TOML cannot spell null, so links the deployment does not model are omitted from the tail — a shorter array leaves the remaining slots unspecified. `initial_joint_positions_rad` is an arm-only array in `joint_names` order. It is inert unless the device declares `arm.init`; that declaration requires exactly one finite value per configured joint.

### Device-level operational keys

These keys describe the device, never the transport, so they survive backend switches:

- `joint_names` — ordered URDF joint names and the exact array order for `arm.init` and `arm.movej`. Omitted defaults to `joint_1..joint_6`; valid for class `arm`/`gimbal` only.

Legacy `[[device.scene.objects]]` input is rejected. Lightweight raster-demo maps live at `<state>/simulator/maps/<map-id>/manifest.json`; vision lines reference the global `<state>/simulator/library` by `source_id`. Configured vision-model output, not binding data or WebUI metadata, is the only source of semantic detections. See the [standalone simulator interface](../develop/interfaces/external/simulator.md).

### Command surface

`[[device.primitive]]` entries bind EOS-owned primitives to a device. `name`, exact SemVer `version`, and `description` are required; `description = ""` is valid. Optional `tier` is `standard` or `operator`. A declaration is admitted against the canonical primitive catalog: the exact identity must exist, belong to the device class, use the catalog's fixed tier, and be executable by the backend type. Advanced primitives additionally require `model_id`, which binds that hardware operation to one exact `[[perception.model]]`; direct primitives reject this key. EOS refuses startup when the referenced model is absent, disabled, or declares a different capability. It never selects another model merely because it has the same capability. Provider health is dynamic: after startup, an unhealthy bound model makes the advanced primitive unavailable without changing its binding. `arm.init` has no call arguments and releases `[device.part].initial_joint_positions_rad` through the arm's joint-motion backend. Every current primitive uses version `1.0.0`. `motion.stop` and `safety.estop` are privileged and implicit, so declaring either is an error. Discovery and dispatch admission derive from this same catalog; northbound clients cannot register devices or primitives.

Reference-aware motion declarations (`base.navigate`, `gimbal.motion`, `arm.movej`, and `arm.movel`) require a non-empty `references` array containing `relative`, `absolute`, or both. An absolute spatial binding also requires `absolute_frame`; `arm.movej` uses its configured joint coordinates and therefore has no frame key. EOS publishes the admitted modes, frame, kind, health, and limits in each device's `Discover.primitives` bindings and rejects an unadvertised reference before backend release.

Required numeric arguments require an inline `limits` table. Every interval has at least one finite `minimum` or `maximum`; optional `minimum_exclusive` and `maximum_exclusive` default to false. Admission rejects missing required, inverted, empty, unknown, or non-finite ranges. The required keys are all four values for `base.velocity`, all three values for `base.navigate`, `pan_rad` and `tilt_rad` for `gimbal.motion`, and `joint_positions_rad` for `arm.movej`. Optional `speed_rad_s`, `speed_m_s`, and `force_n` ranges are declared only when the adapter supports explicit values; omitting their range admits only the command field's omitted/default form. Discovery publishes the resolved table on the device binding, and dispatch checks it before backend release. A positive unbounded speed is represented as `{ minimum = 0.0, minimum_exclusive = true }`.

```toml
[[profile.demo.device.primitive]]
name = "arm.movej"
description = "Move all six demo joints in configured order"
tier = "operator"
limits = { joint_positions_rad = { minimum = -3.14, maximum = 3.14 }, speed_rad_s = { minimum = 0.0, minimum_exclusive = true } }
```

### Direct system speaker

A speaker with `[device.backend] type = "aplay"` requires a `[device.backend.aplay]` table. `alsa_device` is an optional non-empty ALSA PCM route and defaults to `default`. `max_stream_bytes` is an optional positive decoded-asset bound and defaults to 16777216. Both tables are strict-keyed.

```toml
[profile.site.device.backend]
type = "aplay"

[profile.site.device.backend.aplay]
alsa_device = "default"
max_stream_bytes = 16777216
```

The backend is native and owned by `eosd`; there is no speaker ROS 2 device or separate playback service. `speaker.play@1.0.0` resolves one EOS-readable audio asset URI, validates the bounded audio, and owns playback and cancellation. `speaker.speak@1.0.0` is an advanced EOS primitive whose declaration requires `model_id` naming an enabled `audio.synthesis` model. Its discovered health covers that exact model-to-speaker dependency chain. See [Aplay speaker backend](../develop/ext/backends/host.md#aplay-speaker-backend).

The demo profiles use system-default host audio: ALSA microphone capture and the native `aplay` speaker both select `alsa_device = "default"`. WebUI camera ingress never supplies audio, and a missing host device is reported rather than replaced by a synthetic source.

### ROS 2 transport

A device with `[device.backend] type = "ros2"` carries a `[device.backend.ros2]` table: `domain_id` (0..232), `namespace` (the side node's namespace), `base_frame`, `telemetry_timeout_ms`, `command_timeout_ms`, optional `pick_timeout_ms`, optional camera `max_image_bytes` and `depth_scale`, plus strict-keyed endpoint groups. Every device owns a separate rclcpp context, node, and executor, so one EOS process may attach devices to different DDS domains without sharing the global ROS context. Every endpoint is `{kind = "topic", name, message}`; only `topic` is implemented. Each group carries exactly one standard message type; vendor protocols stay in a deployment-owned graph-edge adapter:

```text
joint_states     sensor_msgs/JointState          joint telemetry (required)
pose_query       geometry_msgs/PoseStamped       EE pose telemetry (required)
stop             std_msgs/Empty                  motion.stop (required)
estop            std_msgs/Empty                  safety.estop (required)
movej            std_msgs/Float64MultiArray      primitives arm.init and arm.movej
movel            std_msgs/Float64MultiArray      primitive arm.movel
pick             std_msgs/String                 arm.pick command/object JSON
command_result   std_msgs/String                 terminal JSON result by command_id
command_cancel   std_msgs/String                 exact command_id to cancel
gripper_command  std_msgs/Float64MultiArray      [action, force_n or -1]
gripper_state    std_msgs/Float64MultiArray      [position_pct, state, force_n or -1]
image            sensor_msgs/CompressedImage     bounded JPEG camera feed
depth_image      sensor_msgs/Image               optional aligned 16UC1 depth feed
health           std_msgs/Bool                   optional adapter health heartbeat
```

A declared command whose endpoint group is missing fails the launch naming device and group. Every cancellable ROS 2 primitive also requires `command_result` and `command_cancel`. Array commands carry `command_id` and primitive in their layout label; `arm.pick` publishes JSON containing `command_id` and the validated `object_id`. The graph-edge node must durably deduplicate the ID, publish one correlated terminal result, and never retry actuation. Use `demo.toml` as the public structural example when authoring a deployment-owned profile.

The arm requires `joint_states`, `pose_query`, `stop`, and `estop`; the gripper requires `gripper_state` and `estop`; the ROS 2 camera requires `image`. When `depth_image` is configured, EOS waits for RGB and depth messages with the same timestamp, requires matching frame IDs and dimensions, and losslessly encodes `16UC1` depth as PNG. This matches the aligned RGB/depth publications from the `dep_cam` node. `depth_scale` defaults to `0.001`. `telemetry_timeout_ms` is 1..60000, `command_timeout_ms` is 1..300000 and defaults to 30000, optional `pick_timeout_ms` is 1..300000 and otherwise inherits `command_timeout_ms`, and `max_image_bytes` is 1..16777216. A command timeout is terminal and is never followed by another publication.

`health` is the only required feed class. The data classes (`geometric`, `proprioception`, and `sensor`) are optional and should be omitted when the backend does not produce them. An omitted class defaults to 0 Hz. Unknown keys anywhere in a binding are rejected at parse time — a typo or obsolete `max_bytes` entry fails launch instead of being silently ignored.

## Feed rates and admission

Each present `[device.feeds.<class>]` table contains only `max_rate_hz`. At startup, the bridge compares the backend's declared publication rate with that configured limit and refuses an adapter that exceeds it. Payload layout, serialization size, and buffer capacity are implementation concerns owned by the adapter or transport boundary, so custom backends do not need to reproduce compiler-dependent sizes in deployment TOML.

## Direct Camera Perception

Any of `[perception.stream] human = true`, `human_pose = true`, or `face = true` enables the process-owned camera loop using the `vision.human` role bound by `camera_pose.detection_model_id`; it publishes tracked people to `detection.snapshot`. `human_pose` and `face` additionally enable continuous enrichment workers through `camera_pose.model_id` and `camera_face.model_id`. When those switches are false, the configured role bindings remain available for one-shot exact-frame skill inference. Retained frames keep the JPEG and its timestamp-aligned depth PNG and scale together. Face registration remains the `face.register` primitive, whose hardware declaration binds its own WebSocket model client. Missing, disabled, or wrong-capability models required by an enabled stream fail startup; EOS never substitutes another model.

`[perception.camera_pose].gimbal_part_id` defaults to `default`. When that same-robot gimbal has telemetry, EOS retains its pan, tilt, rates, and monotonic sample time with each camera source frame. Fixed-camera deployments simply omit `source_gimbal` when no gimbal sample is available.

`[perception.camera_object]` declares demand-driven `robots/{robot_name}/cameras/{camera_device_id}/detections/state` resources for admitted cameras. Its exact model must be enabled with `vision.object_detection`; reads, watches, and skill subscriptions share one lazily started pump per camera. `camera_part_id` selects the default for skills that omit a camera ID. The configured default also backs the legacy `robots/{robot_name}/detection/head/state` alias. Omitting the table declares no object-detection resources. Human, human-pose, and face results remain at `robots/{robot_name}/detection/state`.

## Runtime safety policy

`etc/eos/safety.toml` records future runtime-software safety inputs: `[limits]` (velocity, acceleration, torque, workspace keep-out — the action gate's H(q) numeric bounds), `[estop]` (the external estop subscription socket and the on-assert behavior, `block` or `shutdown`), and `[gate]` (`loop_deadline_ms`, the release loop's own runtime deadline; a miss must fail closed and fire the estop latch). Hardware-level protection — the estop device itself, hardware watchdogs, drive-level limits — is outside EOS by design and has no key here. This file is not yet read by the demo-scale runtime; when implemented, these inputs harden the existing Dispatcher rather than creating another release module.

## Rules

- The `[mode]` section contains exactly one `[[mode.binding]]`. It carries `backend` (relative to the etc root) and required `profile`. `[mode.primitives] expose_operator` defaults false. The committed process configuration selects the public `demo` loopback and profile-authoring example; deployments select their own site binding explicitly.
- Separately deployed model providers are outbound-only. EOS validates their configuration at launch but never downloads weights, starts a model server, substitutes a built-in provider, or treats remote availability as part of the actuator release path.
- Ignored site-local bindings created under the old primitive contract must rename arm declarations to `arm.movej`/`arm.movel`, add `description` to every primitive declaration, replace ROS 2 arm command endpoints with `movej`/`movel`, and update graph-edge adapters for the array layouts in [southbound primitives](../develop/interfaces/southbound/primitives.md). The previous names and low-level endpoints have no compatibility aliases.
- Older bindings must also remove `[robot].id` and every device `id` before launch.
- The public `demo` profile is the seven-device loopback and profile-authoring example. Deployment-owned bindings carry site endpoints and hardware details.
- Any file that would differ between two sites belongs outside the tree.
