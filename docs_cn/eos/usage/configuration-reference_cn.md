# Configuration

本参考文档说明可选 C++ EOS 进程配置。默认 Rust v2 运行时使用[节点图契约](../interfaces/hardware_cn.md)和[v2 服务进程指南](../develop/services_cn.md)。在 C++ 侧链中，`etc/` 是参考配置根目录，包含包内默认值与 schema，独立于已安装程序。部署通过 `EOS_ETC` 或 `--etc` 指向自己的根目录；若无法解析，服务端启动失败并报告解析后的路径，不使用编译进程序的后备端点。

`[gateway]` 只包含 `host` 与 `grpc_port`，默认监听 `127.0.0.1:16200`；`16004` 仍保留给外部 simulator。`--host` 与 `--grpc-port` 使用相同校验覆盖配置。该 listener 通过 gRPC 提供 `GatewayService`、`SkillService` 与 `WorkflowService`。EOS core 不再直接接受 HTTP；HTTP client 使用独立启动的 adapter。

`etc/ext/north-http/config.toml` 的严格 `[north_http]` table 独立管理 HTTP `host`/`port`、EOS gRPC `upstream`、`allow_remote`、精确 CORS origin、body/concurrency limit、readiness/request deadline 与 SSE heartbeat。默认从 `127.0.0.1:16201` 连接 `127.0.0.1:16200`；非 loopback bind 必须显式设置 `allow_remote = true`。CLI `--host`、`--port`、`--upstream`、`--log-level` 覆盖文件。admitted upstream 构造一个 process-wide gRPC channel 和一个 Gateway/Skill/Workflow stub set；它不会按 route 或 request 复制。参见 [north HTTP configuration module](../develop/ext/north_http/config_cn.md) 与[complete adapter contract](../interfaces/external/http_cn.md)。

同一文件的严格 `[action_list_audit]` table 管理 `enabled`、`rotation_size_bytes`、`rotation_files` 与 `reconnect_delay_ms`。缺少 table 时为兼容而禁用 audit；默认依次为 `false`、16 MiB、8 files 与 1000 ms，而 committed reference 显式设置 `enabled = true`。数值上限为 256 MiB、64 files 与 60,000 ms，overflow-safe size/count product 上限也是 256 MiB。journal directory 固定派生为 `${state}/north-http/action-list-audit`，不接受 path key。audit 复用进程 Workflow stub，并保持 external 与 non-authoritative。

`[log]` 控制同步日志 sink。`level`、`file_level` 和 `jsonl_level` 分别设置 stderr、路由文本文件和 JSONL 的阈值。各服务写入自己的 `var/log/eos/runs/<launch-id>/<service>/` 目录；systemd journal 记录受管理单元的启动与退出。服务将 `events.jsonl`、`main.log`、`runtime-<module>.log`、`ext-<module>.log` 与 `thirdparty.log` 直接写入 `<service>/`，而 `var/log/eos/latest` 指向最近初始化的服务运行目录；仅设置共同的 `EOS_LAUNCH_ID` 时才共享启动目录。默认 `overwrite = false` 时，每次启动以唯一 ID 保留；`overwrite = true` 时复用 `runs/current`，并在写入新输出前清空整个 launch（包括日志与附件），不影响 retained run。`rotation_size_bytes` 和 `rotation_files` 默认是 64 MiB 与五份历史文件。`dedup` 启用相同事件抑制，`dedup_window_s` 是正数 quiet period。`save_attachments` 将选定 payload 保存到扁平的 `var/log/eos/runs/<launch-id>/attachments/`，也可通过 `var/log/eos/latest/attachments/` 访问。文件使用 `asr-<uuid>.wav`、`tts-<uuid>.<ext>`、`vision-<uuid>.jpg` 等 category prefix，不创建 category subdirectory；attachment event 包含 `category` 与 `relative_path`。这些设置不控制 `var/lib` 下的 durable workflow 或 ActionList audit journal。

进程启动配置属于 `etc/runtime/stack.toml`，不属于 `eos.toml`。其显式 `modules` 列表引用 `etc/ext/<name>/module.toml`；模块可声明服务和所选 EOS binding 的 `[[binding]]` 关系。只安装包不会启用它，必须由已选 stack 引用。可选的 `package_root` 指定非默认软件包目录。systemd 用户启动器和 `eosd` 都根据 `isos.lock` 验证所选包的身份、版本与安装内容。启动器管理普通服务与持久驱动；图变更需重启后生效。`eosd` 会拒绝旧的 `[launch.prelaunch]`、`[launch.adapter]` 配置并给出迁移提示。参见[启动说明](../debug/launch_cn.md)。

锁定的技能包模块可设置相对于已校验安装版本的 `skill_directory = "lib/island/skills"`。工作进程访问硬件时，需要 `[[skill_capability]]` 声明 `skill`、`skill_version`、`slot`、`argument_pointer`、`robot_id`、`device_class`、`primitive` 和 `primitive_version`。JSON 指针从执行参数读取稳定的部件 ID；EOS 将其解析到已准入的设备，只为当前执行授权指定的 primitive 和设备。未声明或无法解析的能力会被拒绝。

`[[perception.model]]` 配置独立部署的感知模型服务的出站客户端。每个条目需要进程内唯一的 `id`、受支持的 `capability` 和带数字 IPv4 或 IPv6 主机的绝对端点。`transport` 默认为 `http`；`websocket` 要求 `ws://` 端点、JSON wire format 和 vision capability。HTTP 可使用 `json` 或 `multipart`；`http_method`、`file_field`、`fields` 与 `api_key_env` 仅适用于 HTTP。`adapter` 选择 `adapters/eos-cxx/ext/models` 实现的 provider 映射，默认为 `canonical`。`timeout_ms` 默认为 3000，范围上限为 300000；`max_request_bytes` 与 `max_response_bytes` 默认各 16 MiB。`provider_model_id` 只交给 adapter。可选 `health_endpoint` 仍是独立的 `http://` 健康探针；省略时健康为 unavailable。

[perception.stream] 只接受 `vad`、`human`、`human_pose` 与 `face`。每个开关都是 boolean，默认 `false`。已提交配置选择 `vad = true`、`human = true`、`human_pose = true` 与 `face = false`；human stream 提供带追踪 ID 的检测框，human-pose stream 使用相同源帧补充规范化 pose 与 action 语义。

可选的 `enabled` 默认为 `true`，且仅在启动时读取。禁用条目仍会完整校验并参与进程级模型 ID 去重，但不会被调用；EOS 不按 capability 自动替换模型，也没有 `local_fallback`。

规范 ASR、TTS 与 VAD request 的 `stream` 默认为 `false`；buffered call API 明确拒绝 `stream = true`。

已提交参考配置声明八个逻辑客户端。Human 与 human-pose 使用持久 WebSocket；ArcFace registration 使用独立 WebSocket client；ArcFace recognition、Audio8、multipart ASR、VAD 与 health probe 使用 HTTP。Provider mapping 仍由 `adapter` 选择，runtime 没有 built-in inference fallback。

原生技能在 `eos.toml` 之外选择。锁定的外部软件包通过启用模块的 `skill_directory` 进入 EOS，该目录必须位于已校验的安装版本内。EOS 为每次外部技能执行启动工作进程。源码构建仍可使用本地 `skills/` 目录；没有选中软件包技能目录时，`--skills-dir PATH` 或 `EOS_SKILLS` 可指定另一个绝对路径。技能动态库不会由 EOS 上传、监视、修改或持久化。ABI 和构建流程见[技能开发](../interfaces/skills/native/README_cn.md)。

## Layout

```
etc/
├── eos/
│   ├── eos.toml           # EOS 设置和 [[mode.binding]]
│   ├── eos.demo.toml      # 无硬件示例
│   ├── backends/          # 内置及部署方 binding profile
│   └── safety.toml        # 预留安全策略
├── runtime/
│   └── stack.toml         # 显式模块列表与核心进程图
└── ext/
    ├── simulator/
    │   ├── module.toml    # 模拟器服务和 binding 关系
    │   └── config.toml    # 模拟器配置
    ├── north-http/
    │   ├── module.toml    # 客户端服务声明
    │   └── config.toml    # HTTP listener 配置
    └── <name>/            # 外部包拥有的配置与声明
```

## Binding files

Every binding file is a **profiled whole-robot description**: one `[profile.<name>]` table per environment, with robot identity in `[profile.<name>.robot]` and devices in `[[profile.<name>.device]]`. `eos.toml` selects exactly one `(file, profile)` per EOS process. `robot_id` is a stable configured binding key（省略时才生成进程生命周期 UUIDv7），`robot_name` 是 fleet-unique 的 human route name；资源 URI 使用后者。Device IDs 按 binding 顺序独立生成 `<class>-<zero-based-index>`。旧的 device `id` key 会以迁移错误失败。The file carries what the agent chain consumes — hardware metadata, devices, command surfaces, admitted numeric ranges, feed rates, and transports. URDF and controllers remain the physical authority for kinematics, geometry, reach, payload, and hardware protection; the binding mirrors caller-visible numeric command intervals so registry discovery and dispatch use the correct device envelope. Each `[[device]]` splits what a device is from how EOS reaches it:

- `class` — the semantic device kind: `arm`, `base`, `gimbal`, `cam`, `mic`, `gripper`, or `speaker`. The class selects the northbound resource family and the part contract.
- `[device.backend]` — the forwarding adapter: `type = "demo"`, `"vendor"`, `"ros2"`, `"alsa"`, or `"aplay"`. The registry discovers the `(type, class)` pair from this configuration and constructs only that adapter. Demo implements all device classes; the private deployment adapter implements `base` and direct `cam`; ROS 2 implements `arm`, `gripper`, and `cam`; `alsa` implements `mic`; `aplay` implements `speaker` and adds a `[device.backend.aplay]` table.

Device keys: `class` (required); `part_id` (default `"default"`), `description`, `tags`, and `end_effector` (free-form deployment metadata, e.g. `"none"`) optional. Do not configure `id`. The `[device.part]` table carries per-class static scalars, all optional with the defaults below; a key a class does not define is a parse error naming the file and key:

```text
base:    footprint_length_m (0.5), footprint_width_m (0.3)
gimbal:  fov_rad (1.4), range_m (5.0)
cam:     fov_rad (1.4 for demo; unknown for ROS 2 unless configured)
arm:     link_lengths_m ([0.18, 0.16, 0.12]), mount_x_m (0.0), mount_y_m (0.0), initial_joint_positions_rad (unset)
mic, gripper, speaker: none yet
```

`link_lengths_m` holds up to six non-negative link lengths in link order. TOML cannot spell null, so links the deployment does not model are omitted from the tail — a shorter array leaves the remaining slots unspecified. `initial_joint_positions_rad` is an arm-only array in `joint_names` order. It is inert unless the device declares `arm.init`; that declaration requires exactly one finite value per configured joint.

### Device-level operational keys

These keys describe the device, never the transport, so they survive backend switches:

- `joint_names` — ordered URDF joint names and the exact array order for `arm.init` and `arm.movej`. Omitted defaults to `joint_1..joint_6`; valid for class `arm`/`gimbal` only.

旧的 `[[device.scene.objects]]` 输入会被拒绝。轻量栅格 Demo 地图位于 `<state>/simulator/maps/<map-id>/manifest.json`，视觉 line 通过 `source_id` 引用全局 `<state>/simulator/library`；只有配置的视觉模型输出可以生成语义 detection，binding 数据或 WebUI metadata 均不能生成。见[独立模拟器接口](../interfaces/external/simulator_cn.md)。

### Command surface

`[[device.primitive]]` 条目把 EOS 自有 primitive 绑定到设备。`name`、精确 SemVer `version` 与 `description` 必填，`description = ""` 合法；可选 `tier` 为 `standard` 或 `operator`。声明必须存在于规范 catalog、属于该设备类别、使用固定 tier，且 backend 能执行。primitive 版本必须精确匹配；`base.navigate` 支持 `1.0.0`、`1.1.0` 与 `1.2.0`；后两者分别接受数字导航步态与配置的步态别名。`base.gait@1.0.0` 切换独立步态。Vendor base 接纳 `base.posture`、`base.gait` 与相对或绝对模式的 `base.navigate`；其 API 没有 SI 速度命令，因此不能声明 `base.velocity`。`motion.stop` 与 `safety.estop` 为隐式 privileged primitive，不得显式声明。发现与 dispatch admission 来自同一 EOS 自有 catalog；northbound 客户端不能注册设备或 primitive。

支持 reference 的运动声明（`base.navigate`、`gimbal.motion`、`arm.movej`、`arm.movel`）需要非空 `references` 数组，值为 `relative`、`absolute` 或两者。接纳 absolute 的空间运动还需要 `absolute_frame`；`arm.movej` 使用配置的关节坐标，不需要 frame。EOS 在各设备的 `Discover.primitives` binding 中发布已接纳的模式、frame、kind、health 与 limits，并在 backend release 前拒绝未公布的 reference。

`base.navigate@1.2.0` 与 `base.gait@1.0.0` 的 primitive 声明必须包含非空 `gait_aliases` 表。键是对外接纳的别名，值是 Island-Base 的规范步态名。导航只允许 `walk` 和 `perceived_stairs`；独立的 `base.gait` 允许 `walk`、`slope`、`obstacle`、`stairs`、`accumulated_stairs`、`accumulated_steep_stairs`、`l_walk`、`mountain` 和 `quiet`。EOS 在 dispatch 时拒绝未知别名，并通过 `Discover.parameters.properties.gait.enum` 发布配置的键。独立步态切换需要 Island-Base 处于踏步状态，并等待控制器确认。

```toml
gait_aliases = { normal = "walk", stairs_approach = "perceived_stairs" } # base.navigate@1.2.0
```

必填数值参数需要内联 `limits` table。每个区间至少有一个有限 `minimum` 或 `maximum`；可选 `minimum_exclusive` 与 `maximum_exclusive` 默认为 false。必需键包括 `base.velocity` 的四个值、`base.navigate` 的三个值、`gimbal.motion` 的 `pan_rad` 与 `tilt_rad`，以及 `arm.movej` 的 `joint_positions_rad`。可选的 `speed_rad_s`、`speed_m_s` 与 `force_n` 只在 adapter 支持显式值时声明。Discovery 在设备 binding 上发布解析后的范围，dispatch 在 backend release 前校验。

```toml
[[profile.demo.device.primitive]]
name = "arm.movej"
description = "Move all six demo joints in configured order"
tier = "operator"
limits = { joint_positions_rad = { minimum = -3.14, maximum = 3.14 }, speed_rad_s = { minimum = 0.0, minimum_exclusive = true } }
```

### Direct system speaker

A speaker with `[device.backend] type = "aplay"` requires a `[device.backend.aplay]` table. `alsa_device` is an optional non-empty ALSA PCM route and defaults to `default`; `max_stream_bytes` is a positive decoded-asset bound and defaults to 16777216. Both tables are strict-keyed.

```toml
[profile.site.device.backend]
type = "aplay"

[profile.site.device.backend.aplay]
alsa_device = "default"
max_stream_bytes = 16777216
```

The backend is native and owned by `eosd`; there is no speaker ROS 2 device or separate playback service. `speaker.play@1.0.0` resolves and validates one EOS-readable audio asset, then owns playback and cancellation. `speaker.speak@1.0.0` is an advanced primitive; its `[[device.primitive]]` declaration requires `model_id` naming an enabled `audio.synthesis` model. The reference Audio8 model uses `adapter = "audio8-speech-v1"` and a 60000 ms deadline. See [Aplay 扬声器后端](../develop/ext/backends/host_cn.md#aplay-扬声器后端).

默认 `demo` profile 使用 loopback 麦克风和扬声器。可选 `demo-host` profile 使用系统默认音频：ALSA 麦克风采集与原生 `aplay` 扬声器都选择 `alsa_device = "default"`。WebUI camera ingress 不提供音频。

### ROS 2 transport

`[device.backend] type = "ros2"` 的设备带有严格键控的 `[device.backend.ros2]` 表，其中包含 `domain_id`（0..232）、side node 的 `namespace`、`base_frame`、`telemetry_timeout_ms`、`command_timeout_ms`，以及 camera 可选的 `max_image_bytes` 和 `depth_scale`。每个设备拥有独立的 rclcpp context、node 和 executor，因此同一 EOS 进程可以连接不同 DDS domain 中的设备。 每个 endpoint 都是 `{kind = "topic", name, message}`；目前只实现 `topic`。Vendor protocol 应留在部署方拥有的 graph-edge adapter 中：

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

已声明 command 缺少对应 endpoint group 时，启动会失败并指出 device 与 group。 每个可取消的 ROS 2 primitive 还必须同时声明 `command_result` 和 `command_cancel`。Graph-edge node 必须持久去重 `command_id`、发布一次关联的终态 结果，且不得重试执行。编写其他部署方 profile 时，以 `demo.toml` 作为公开的 结构示例。

Arm 需要 `joint_states`、`pose_query`、`stop` 和 `estop`；gripper 需要 `gripper_state` 和 `estop`；ROS 2 camera 需要 `image`。配置 `depth_image` 后， EOS 等待时间戳相同的 RGB 与 depth message，要求 frame ID 与尺寸一致，并将 `16UC1` depth 无损编码为 PNG。`depth_scale` 默认为 `0.001`。 `telemetry_timeout_ms` 范围为 1..60000，`command_timeout_ms` 范围为 1..300000、默认值为 30000，`max_image_bytes` 范围为 1..16777216。Command timeout 是终态，之后不会再次发布该 command。

`health` is the only required feed class. The data classes (`geometric`, `proprioception`, and `model`) are optional and should be omitted when the backend does not produce them. An omitted class defaults to 0 Hz. Unknown keys anywhere in a binding are rejected at parse time — a typo or obsolete `max_bytes` entry fails launch instead of being silently ignored.

## Feed rates and admission

Each present `[device.feeds.<class>]` table contains only `max_rate_hz`. At startup, the bridge compares the backend's declared publication rate with that configured limit and refuses an adapter that exceeds it. Payload layout, serialization size, and buffer capacity are implementation concerns owned by the adapter or transport boundary, so custom backends do not need to reproduce compiler-dependent sizes in deployment TOML.

## Direct Camera Perception

`[perception.stream]` 的 `human`、`human_pose` 或 `face` 中任一开关为 `true` 都会启用进程内 camera loop，使用 `camera_pose.detection_model_id` 绑定的 `vision.human` role，并把 tracked human 发布到 `detection.snapshot`。`human_pose` 与 `face` 还会分别通过 `camera_pose.model_id` 和 `camera_face.model_id` 增加连续 enrichment worker。这些开关为 false 时，已配置 role binding 仍可用于 one-shot exact-frame skill inference。Retained frame 同时保存 JPEG、时间戳对齐 depth PNG 与 scale。Face registration 仍是 `face.register` primitive，其 hardware declaration 独立绑定 WebSocket model client。启用 stream 所需模型缺失、禁用或 capability 错误会导致启动失败；EOS 不替换模型。

`[perception.camera_object]` 声明按需生成的 `robots/{robot_name}/detection/head/state` resource。其精确 model 必须启用并具有 `vision.object_detection` capability；read、watch 与 skill subscription 共享一个延迟启动的 pump。省略该 table 时不会声明 head-detection resource。Human、human-pose 与 face result 仍使用 `robots/{robot_name}/detection/state`。

## Runtime safety policy

`etc/eos/safety.toml` records future runtime-software safety inputs: `[limits]` (velocity, acceleration, torque, workspace keep-out — the action gate's H(q) numeric bounds), `[estop]` (the external estop subscription socket and the on-assert behavior, `block` or `shutdown`), and `[gate]` (`loop_deadline_ms`, the release loop's own runtime deadline; a miss must fail closed and fire the estop latch). Hardware-level protection — the estop device itself, hardware watchdogs, drive-level limits — is outside EOS by design and has no key here. This file is not yet read by the demo-scale runtime; when implemented, these inputs harden the existing Dispatcher rather than creating another release module.

## Rules

- The `[mode]` section contains exactly one `[[mode.binding]]`. It carries `backend` (relative to the etc root) and required `profile`. `[mode.primitives] expose_operator` defaults false. The committed process configuration selects the public `demo` loopback and profile-authoring example; deployments select their own site binding explicitly.
- Perception model clients are outbound-only. EOS validates their configuration at launch but never downloads weights, starts a model server, or treats remote availability as part of the actuator release path.
- Ignored site-local bindings created under the old primitive contract must rename arm declarations to `arm.movej`/`arm.movel`, add `description` to every primitive declaration, replace ROS 2 arm command endpoints with `movej`/`movel`, and update graph-edge adapters for the array layouts in [the southbound interface](../interfaces/southbound/driver_cn.md). The previous names and low-level endpoints have no compatibility aliases.
- Older bindings must also remove `[robot].id` and every device `id` before launch.
- The public `demo` profile is the seven-device loopback and profile-authoring example. Deployment-owned bindings carry site endpoints and hardware details.
- Any file that would differ between two sites belongs outside the tree.
