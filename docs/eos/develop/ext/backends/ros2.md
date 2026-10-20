# ROS 2 backend

[Chinese](../../../../../docs_cn/eos/develop/ext/backends/ros2_cn.md)

[Extensions and backends overview](../README.md) defines the shared backend boundary. The optional ROS 2 adapter maps configured ROS topics and services into EOS device feeds and commands; it does not change canonical resource or primitive semantics.

Each configured device is admitted only when its EOS class, ROS mapping, required endpoints, message types, and units are compatible. Unknown device classes, missing required mappings, malformed names, incompatible message types, and unsupported commands reject admission or the individual operation rather than selecting a guessed mapping.

Incoming messages are converted to backend-neutral samples with their source timing and declared units. Independently arriving topics retain independent freshness. A resource that requires a paired or complete state is unavailable until every required member has arrived; stale, oversized, malformed, or out-of-order data is not presented as a fresh complete sample. Optional fields remain absent when ROS supplies no value. Unknown gripper force or another unmeasured quantity stays unknown rather than becoming zero.

Manipulator bindings may optionally map `arm_status` to a `std_msgs/String` topic. EOS accepts the versioned arm-status envelope only when its timestamp, controller state, ordered joint names, and every present diagnostic are valid. The controller sample expires independently from joint and pose telemetry. Vendor register values whose engineering units are not contracted remain explicitly named `*_raw`; they are never projected as velocity, effort, torque, amperes, degrees, or volts.

Outgoing commands arrive only through dispatch after schema, target, range, lease, and safety checks. Every motion carries its durable `command_id`. Normal target commands wait for the matching `command_result` JSON response; stream target commands instead complete at successful publication. Transport unavailability, middleware rejection, timeout, cancellation, and shutdown return an explicit terminal result. EOS does not infer physical execution merely because a message was published and never republishes a command after any ambiguous outcome.

For `arm.pick`, the adapter publishes a `std_msgs/String` JSON envelope carrying the `command_id` and schema-valid `obj:*` identifier. The deployment-owned receiver resolves that ID against its current world state, owns freshness, pickability, planning and grasp execution, durably deduplicates the command, and reports completion on `command_result`. `command_cancel` carries only the exact ID; repeated cancellation must not affect a later command.

`command_result` is a `std_msgs/String` JSON object such as `{"command_id":"pick-19","status":"succeeded"}`; failures may add `reason`. The accepted status values are `succeeded`, `failed`, `cancelled`, `timed_out`, and `outcome_unknown`. A missing result at `command_timeout_ms` (or `pick_timeout_ms` for `arm.pick` when set) becomes `outcome_unknown`, and EOS does not publish the command again.

A ROS 2 gimbal may be explicitly mapped to two joints of an admitted ROS 2 arm. Its `gimbal_joint_map` names the arm part and the distinct pan and tilt joints; `tilt_direction` must be `1` or `-1` and defaults to `1`. The gimbal must repeat the arm's ordered six-joint list and its DDS domain, namespace, timeout, joint-state, MoveJ, stop, estop, and health endpoints exactly. EOS accepts a gimbal command only after a fresh complete joint sample, preserves the four unmapped measured positions, replaces the mapped positions, and publishes one full MoveJ target. The configured tilt direction is applied symmetrically to commands and position/velocity telemetry. This mode intentionally shares one external arm controller; it does not create a second low-level actuator protocol.

Heartbeat or endpoint loss marks the affected device unhealthy without inventing replacement telemetry. Stop, lease expiry, disconnect, and process shutdown request the safest mapped zero or halt operation; inability to deliver that operation remains observable and does not clear an emergency state.

The adapter is built only when ROS 2 support is enabled. Qualification requires unit or stubbed integration checks for conversion and failure mapping plus hardware-in-loop or a representative ROS graph for topic/service compatibility, freshness, cancellation, and shutdown. The general build and test commands are in [EOS README](../README.md).

## Illustrative site-local profile

The repository does not ship a `live.toml` profile. A deployment may create an ignored site-local ROS 2 arm binding under its configuration root. It should give the robot a stable `robot_id`, a fleet-unique `robot_name`, a `robot_desc`, and a description for every device. The following illustrative surface assumes only `arm-0`, with six ordered joints named `joint_1` through `joint_6`; it is not a complete robot template and advertises no base, gimbal, camera, microphone, gripper, or speaker.

### Primitive surface

Only `arm.movej` is declared. The payload must contain exactly six finite `joint_positions_rad` values in configured joint order, each in `[-3.14, 3.14]`. The profile does not declare a `speed_rad_s` limit, so callers must omit that optional field and allow the ROS 2 side to choose its default.

`arm.init` is unsupported because no declaration or initial joint pose exists. `arm.movel` is unsupported even though pose telemetry is configured. All base, gimbal, gripper, and speaker primitives are unsupported because their target devices do not exist. `motion.stop` is implicit for `arm-0`; `safety.estop` is implicit and process-wide, affecting the one admitted arm.

### ROS 2 data and release conditions

The arm uses DDS domain 0 and namespace `/eos`. Joint state arrives on `/robot/arm/joint_states`; pose telemetry arrives on `/robot/arm/pose`; joint commands publish to `/robot/arm/movej`; terminal results arrive on `/robot/arm/command_result`; targeted cancellation publishes to `/robot/arm/command_cancel`; stop and estop publish to `/robot/arm/stop` and `/robot/arm/estop`. Telemetry older than 1500 ms is stale. Proprioception and health publication are each capped at 10 Hz; no explicit ROS health-heartbeat topic is configured, so health is derived from adapter/telemetry state rather than a separate heartbeat.

Startup rejects missing or incompatible required endpoint definitions and rejects the profile when ROS 2 support is not compiled. At call time, ROS 2 readiness, a live command subscriber, payload validity, and the non-latched runtime are required. Normal `arm.movej`, `arm.movel`, and `gimbal.motion` commands carry a command ID and wait for the deployment node's correlated terminal response. A stream request appends `;stream=true` to the existing array correlation label, publishes once, and returns accepted without registering a terminal wait; default requests preserve the previous label format. Late result messages for stream IDs are ignored. The graph-edge adapter must interpret stream mode as a coalescible target update rather than an ordinary overlapping blocking command. It must durably deduplicate requests and never actuate a repeated ID. Transport loss, stale telemetry, no subscriber, timeout, stop, estop, and shutdown remain explicit terminal conditions.

Use the committed `etc/eos/backends/demo.toml` as the structural profile example, then apply the ROS 2 mapping and admission rules documented above.
