# Develop the MuJoCo benchmark package

<!-- docs-tree:start -->
**Location:** [Docs](../../../README.md) / [EOS](../../README.md) / [Develop](../README.md) / [Ext](README.md) / **mujoco**

<details>
<summary>Docs tree</summary>

- [docs/](../../../README.md)
  - [eos/](../../README.md)
    - [develop/](../README.md)
      - [ext/](README.md)
        - [backends/](backends/README.md)
        - [north_http/](north_http/README.md)
        - [tools/](tools/README.md)
        - **mujoco.md**

</details>
<!-- docs-tree:end -->

[中文](../../../../docs_cn/eos/develop/ext/mujoco_cn.md) · [Package development](README.md) · [Run it](../../usage/mujoco.md) · [Design](../../design/mujoco.md)

`isos-mujoco-sim` is an independent C++20/CMake repository beside Island-eos. It links the installed MuJoCo 3.16.0 through `MUJOCO_ROOT`; no EOS core target links MuJoCo. Its `eos-mujoco-sim` executable implements the existing `eos.simulator.v1.SimulatorService` over `<run>/simulator.sock`. Its separate, owner-only benchmark service runs over `<run>/benchmark.sock`. The repository is [Island-Team/isos-mujoco-sim](https://github.com/Island-Team/isos-mujoco-sim).

## Package map

| Path in the package | Ownership |
| --- | --- |
| `assets/generate_scene.py`, `assets/scene.xml` | Composed mobile manipulator, camera, table, cube, tray, and seeded obstacles |
| `assets/menagerie/`, `assets/MODELS.md` | Pinned UR5e and Robotiq assets, revision, and retained licenses |
| `src/engine.*` | Fixed-step MuJoCo state, control, IK, contact observations, rendering, and scoring |
| `src/service.*` | EOS simulator RPC and private benchmark RPC adapters |
| `proto/simulator.proto`, `proto/benchmark.proto` | Pinned EOS simulator contract copy and private benchmark contract |
| `tests/engine.cpp`, `tests/smoke.cpp` | Physical episode and service-level regression checks |
| `LICENSE`, `assets/menagerie/*/LICENSE`, `assets/MODELS.md` | Apache-2.0 package license, retained model licenses, and model provenance |

The imported Menagerie revision is `cda5236f98383f6fbaa4820c6b18dba892cd087f`. Preserve its model licenses and source XML; regenerate the composed scene after changing `generate_scene.py`. An EOS simulator contract change must update the package's pinned `simulator.proto` in the same change.

## Integration boundary

The EOS binding at `etc/ext/mujoco/binding.toml` advertises only base, arm, camera, and gripper. The EOS `mujoco` descriptor reuses the simulator IPC adapters but delegates Cartesian reach and collision decisions to MuJoCo rather than the demo arm's planar check. It gives motion RPCs a longer deadline while retaining short health and observation calls. The package must report completion only when the physical target is reached or an explicit failure occurs; stop and halt must interrupt active motion.

The package's `ReadTelemetry` and `ReadCameraFrame` provide measured joint/base/gripper state and rendered JPEGs. The renderer currently fixes camera output at `640 × 480` and uses headless EGL. Unsupported device IDs and commands fail closed. Do not advertise `arm.pick` until a high-level command executes a physical grasp and reports its actual outcome.

Keep the benchmark service private. It may expose ground truth and seed control to a trusted harness; an agent receives only EOS discovery, observations, and primitive calls. The exact RPC surface belongs in the [simulator interface](../../interfaces/external/simulator.md) and the package protobuf files, rather than being duplicated here.

## Qualification

Build with `cmake -S . -B build -DMUJOCO_ROOT=...`, then run `cmake --build build -j` and `ctest --test-dir build --output-on-failure` in the package. The engine test covers seeded replay, movement completion, workspace rejection, stop/halt, repeated JPEG capture, and a physical seed-42 pickup and placement. The RPC smoke test checks service compatibility, telemetry dimensions, JPEG output, private stepping, and health. EOS config, wrapper, launcher, and demo simulator tests run in the Island build tree.

`scene-view-check` runs the installed MuJoCo `compile` tool against the composed XML without EOS or a display. `tools/scene-view.sh` performs that check before opening `simulate` for visual debugging. It validates scene loading and visible geometry; seeded runtime placement and command behavior remain covered by the engine and RPC tests.

For a full integration check, launch the package and EOS with the MuJoCo HAL profile, call EOS `Discover` and primitives with unique command IDs, and inspect `Score()` on the benchmark socket. Keep a scripted oracle in tests for regression; the deliverable does not include an agent policy.

## Seed-42 regression episode

The deterministic seed-42 episode navigates to the table, closes on the cube, lifts it, transfers it with the arm, and opens over the marked tray. The package test verifies physical lift and final placement. A run through EOS `GatewayService` with an external benchmark client scored navigation, grasp, and placement true. Collision events remain a separate metric; this oracle records arm/table contacts during successful pickup.

All joint arrays follow the six UR5e names in `etc/ext/mujoco/binding.toml`; `arm.movej` uses `reference = "absolute"`. Send a fresh EOS command ID for each row. The harness calls `Step(200)` after closing and again after lifting, then `Step(300)` in the engine test or `Step(500)` in the EOS run after opening to let the object settle.

| Order | EOS primitive | Target |
| --- | --- | --- |
| 1 | `base.navigate` | relative x `0.6` m, y `0`, yaw `0` |
| 2 | `arm.movej` | `[2.7368, -0.4246, -1.1208, -0.4245, 4.7113, -2.1504]` |
| 3 | `arm.movej` | `[2.7604, -0.2680, -1.1200, -0.2037, 4.6546, -2.1504]` |
| 4 | `gripper.motion` | `action = "close"` |
| 5 | `arm.movej` | repeat row 2 to lift |
| 6 | `arm.movej` | `[3.3459, -0.2874, -1.1062, -0.2623, 4.6620, -2.1504]` |
| 7 | `gripper.motion` | `action = "open"` |

The direct EOS run completed with all three goal booleans true at `7.32` simulated seconds and counted `11` contact events. These targets are a regression oracle for seed 42, not a policy for other seeds.

**Related:** [EOS extension guide](README.md) · [simulator interface](../../interfaces/external/simulator.md) · [hardware runtime](../../design/runtime/hardware-legacy.md)
