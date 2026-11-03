# ROS 2 integration for v2 hardware nodes (design note)

[中文](../../../docs_cn/eos/develop/ros2_cn.md) · [V2 hardware development](hardware.md) · [Existing C++ ROS 2 backend](ext/backends/ros2.md)

This page is the target mapping for a future CXX ROS 2 driver port. The current v2 foundation has typed Rust camera, base, and manipulator interfaces and CXX mocks for all three; it does **not** ship a v2 ROS 2 driver. The existing [C++ ROS 2 backend](ext/backends/ros2.md) documents the current runtime and must not be treated as a v2 adapter implementation.

## One ROS 2 adapter per node

Create one adapter registration for each configured `camera`, `base`, or `manipulator` node in the unified `1.0.0` profile. The validated node path, capabilities, and adapter settings provide the route and endpoint configuration; no separate ROS 2 node manifest is used. Each adapter owns its ROS subscriptions, publishers or action clients, health, and stop capability. A camera node may combine RGB and depth only when they are registered to the same frame and associated sample time; otherwise report RGB with `depth: None` or reject a malformed pair. A base node maps odometry into planar pose and velocity, and accepts only its advertised `body_velocity` or controller-handled `relative_pose` mode. A manipulator node maps a complete ordered joint set and optional tool pose, and accepts only its advertised complete joint or controller-handled end-effector target. The Rust HAL does no navigation or IK.

Keep ROS messages, QoS, DDS domain, topic names, action types, and vendor SDK types in the C++ adapter. Convert them to the [typed HAL values](../interfaces/hardware.md) before returning to Rust. Adapter settings must name and validate required endpoints and message types; no guessed topic or unit mapping. Do not share an actuator endpoint between independently addressable nodes unless a single C++ owner serializes all releases and their stop behavior.

## Time, frames, and safety

- Copy a ROS source timestamp into `SampleMeta.source_timestamp_ns` with `clock = Ros`. Set `received_monotonic_ns` from the shared `aether-common::Clock` at receipt. Only local monotonic time is used for freshness and deadlines; ROS time may pause or jump.
- Preserve the actual ROS `frame_id`; do not silently substitute the profile frame. Reject mismatched or unsupported command frames at the adapter boundary. Camera depth must be aligned to the documented RGB dimensions and calibration. Preserve ordered joint names and never fill missing state with zero.
- Publish or invoke a movement command once with its `command_id`. Correlate any terminal result to that exact ID. A transport timeout after release is `outcome_unknown`, so neither Rust nor the adapter automatically resends it. A ROS controller should deduplicate IDs if it can receive repeated requests after reconnect.
- Map `stop_node` and emergency stop to an explicit ROS/controller stop path for each actuator. A stop delivery failure must remain visible. Body velocity validity is a lease: the controller or adapter must command zero when it expires, including on disconnect and shutdown. These are requirements for the future port, not claims about current v2 mock behavior.
- Health and observations are per node. Endpoint loss must degrade only the affected node, and stale or incomplete samples must not be relabeled fresh. The Rust foundation degrades a node on read error; a ROS 2 port must prove this behavior across its endpoint failures.

## Qualification before enabling a port

Use a stub ROS graph to test conversion, frames, source versus receipt time, missing and out-of-order messages, payload bounds, command ID correlation, timeout uncertainty, stop, and disconnect. Follow with a representative ROS graph or hardware-in-loop run for actual QoS and controller behavior. Save output under `logs/test/<run-id>/` and record the graph, ROS distribution, adapter build features, and result summary. Keep the ordinary Rust workspace checks runnable without ROS 2 installed.
