# Backend adapter catalog

<!-- docs-tree:start -->
**Location:** [Docs](../../../README.md) / [EOS](../../README.md) / [Reference](../README.md) / [Backends](README.md)

<details>
<summary>Docs tree</summary>

- [docs/](../../../README.md)
  - [eos/](../../README.md)
    - [reference/](../README.md)
      - [backends/](README.md)

</details>
<!-- docs-tree:end -->

[Setup backend drivers](../../usage/backends.md) · [Platform template](../../templates/backend-platform.md) · [Module template](../../templates/backend-module.md)

Organize website reference pages first by platform, then by concrete module. The table maps the current source and docs; it is a page plan, not a claim that every adapter is enabled in every build. Add a module page whenever its configuration, supported primitives, or failure behavior differs.

| Platform | Modules to document separately | Existing source of truth |
| --- | --- | --- |
| Demo / simulator | base, arm, gripper, gimbal, camera, microphone, speaker; shared simulator client | [Demo backend](../../develop/ext/backends/demo.md), [simulator interface](../../interfaces/external/simulator.md) |
| System / host | ALSA microphone, V4L2 camera, `aplay` speaker | [Host backends](../../develop/ext/backends/host.md) |
| Vendor / native | vendor base, vendor camera; ALSA/V4L2/aplay implementation modules where vendor wrappers are used | [Backend extension boundary](../../develop/ext/README.md), [host behavior](../../develop/ext/backends/host.md) |
| ROS 2 | arm, gripper, camera, mapped gimbal; one page per supported device mapping | [ROS 2 backend](../../develop/ext/backends/ros2.md), [ROS 2 TOML](../../usage/configuration-reference.md#ros-2-transport) |
| External simulator | MuJoCo package adapter and benchmark control process | [MuJoCo setup](../../usage/mujoco.md), [development](../../develop/ext/mujoco.md) |

For each platform, start with a platform page that names dependencies, build admission, and shared health behavior. For each module, use the [module template](../../templates/backend-module.md) to record its exact backend type, profile keys, supported primitives and feeds, units, completion rules, stop behavior, and qualification. Link primitive pages back to the module that implements them. The [southbound contracts](../../interfaces/southbound/README.md) remain authoritative for portable operation shapes.
