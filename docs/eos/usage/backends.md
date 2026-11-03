# Set up backend drivers

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Usage](README.md) / **backends**

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [eos/](../README.md)
    - [usage/](README.md)
      - [arm64.md](arm64.md)
      - **backends.md**
      - [configuration-reference.md](configuration-reference.md)
      - [configure.md](configure.md)
      - [mujoco.md](mujoco.md)
      - [packages.md](packages.md)
      - [repl.md](repl.md)

</details>
<!-- docs-tree:end -->

[Setup](README.md) · [TOML walkthrough](configure.md) · [Backend contract](../interfaces/southbound/driver.md) · [Debug launch](../debug/launch.md)

EOS selects one hardware profile per process. A profile declares semantic devices, primitive limits, feeds, and each device's backend. The driver or external controller owns physical hardware behavior; EOS validates and releases commands through the [southbound boundary](../interfaces/southbound/README.md).

In the default Rust v2 deployment, the verified MuJoCo package contributes `eos.process.backend@v1.0` and hardware nodes to the applied graph. IsOS resolves the package and starts EOS; EOS directly starts, monitors, and stops its simulator child. Use `isos status` to inspect the deployment and `isos run` or `isos restart` to start or replace it. The physical driver and C++ configuration steps below apply only to the opt-in side chain.

## Choose an integration path

| Path | Preparation | Detailed guide |
| --- | --- | --- |
| MuJoCo simulator | Link or install the verified package through IsOS, then select its nodes in the graph | [MuJoCo setup](mujoco.md) |
| Host microphone, camera, speaker | Check ALSA/V4L2 permissions, route, and required utilities | [Host backends](../develop/ext/backends/host.md) |
| ROS 2 robot | Source ROS and the deployment workspace; build a private bridge package outside Island and declare its driver units | [ROS 2 backend](../develop/ext/backends/ros2.md) |
| Vendor or other native protocol | Implement a backend and descriptor; declare supported operations in a site profile | [Backend authoring](../develop/ext/README.md) |

## Create and select a profile

Copy a tracked example into a deployment-owned file as described in [Local deployment files](configure.md#describe-a-live-binding). Use the selected backend package's binding and profile. Put real endpoints, frames, routes, and measured limits in the deployment profile or graph-edge adapter. The selected `eos.toml` contains exactly one `[[mode.binding]]` with a `backend` path relative to the etc root and a required `profile` name. See the [TOML walkthrough](configure.md#select-the-profile).

Declare only primitives the driver actually executes. Each bounded `[[device.primitive]].limits` interval should reflect the hardware authority. Do not copy demo limits to a physical device. EOS rejects unsupported declarations and invalid bindings at startup; the driver must still enforce its physical envelope.

## Prepare dependencies and prove readiness

- For a host device, check that the EOS service account can access the configured ALSA or V4L2 device and that no competing process owns it.
- For ROS 2, source the same ROS distribution and workspace for build, adapter launch, and diagnostics. Check the exact DDS domain, namespace, topics, message types, and terminal result/cancel behavior described in the [ROS 2 mapping](configuration-reference.md#ros-2-transport).
- EOS starts its verified MuJoCo child and waits for its Unix socket before reporting EOS readiness to IsOS.
- For a custom native backend, add its build target, descriptor, parsing/admission, and failure tests as described in [extension development](../develop/ext/README.md).

## Launch and verify

Run `./bin/eosd` with the selected `EOS_ETC` and `EOS_CONTRACTS` roots, then start `./bin/eos-north-http`. Query `http://127.0.0.1:16201/v1/eos/discover` to confirm the expected robot, devices, and primitives. Inspect [logs](../debug/README.md) if a device is missing or unhealthy. Discovery does not prove a physical motion path; qualify normal execution, rejection, stop, estop, timeout, cancellation, dependency loss, and shutdown on the deployment hardware.
