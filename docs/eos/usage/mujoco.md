# Run MuJoCo with EOS v2

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Usage](README.md) / **mujoco**

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [eos/](../README.md)
    - [usage/](README.md)
      - [arm64.md](arm64.md)
      - [backends.md](backends.md)
      - [configuration-reference.md](configuration-reference.md)
      - [configure.md](configure.md)
      - **mujoco.md**
      - [packages.md](packages.md)
      - [repl.md](repl.md)

</details>
<!-- docs-tree:end -->

[中文](../../../docs_cn/eos/usage/mujoco_cn.md) · [Node IDL](../interfaces/node-contributions.md) · [Hardware contract](../interfaces/hardware.md) · [Simulator interface](../interfaces/mujoco.md) · [R06 stop contract](../interfaces/simulator-stop.md)

`island/mujoco-sim@0.1.1` is an IsOS package that runs the MuJoCo simulator as a direct EOS child. It supplies an executable, scenes and a contributed process type; the selected camera, base and manipulator use generic EOS hardware types with the pinned adapter package. The simulator uses an owner-only Unix socket; EOS owns the hardware adapters and Dispatcher. The R05 EOS strict client launches the child without a historical gRPC actuator or benchmark listener; scoped cross-owner R05 qualification passed; managed movement is exposed after same-generation `strict-stop-v1` proof on both sockets. A separate direct-launch evaluator may use the benchmark socket outside the managed deployment; EOS and EGIA never receive ground truth.

## Prepare and run

Build the Aether Rust executables and IsOS, then build a v2 bundle from the [simulator repository](https://github.com/Island-Team/isos-mujoco-sim). Set `MUJOCO_ROOT` to the MuJoCo 3.16.0 installation containing `lib/libmujoco.so.3.16.0`. Link the local bundle and start the IsOS workbench:

```sh
export MUJOCO_ROOT=/absolute/path/to/mujoco-3.16.0
isos init --mode dev
isos pkg link island/mujoco-sim 0.1.1 /absolute/path/to/bundle --manifest /absolute/path/to/bundle/release.json
isos version check
isos serve
```

Build EOS and IsosUI in their own repositories and configure their output paths in IsOS before running `isos version check`.

`isos init` creates the configuration and exits; leave `isos serve` attached to run the manager and workbench. On the IsosUI Graph page, choose the installed `sim-hardware` node set, select `pick-place` or `navigation`, preview its nodes, then Validate and Apply the revision. The package process node uses `core=eos`, `layer=process`, `group=mujoco`; the hardware nodes use the generic camera/base/manipulator groups. The preset connects the camera to a generic EOS camera gate. It does not create the legacy `isos.ui.vision@v1.0` node or a `preview` edge. Installation and preview do not edit the applied graph; Apply does. Start the applied revision with **Run**, or use `isos run` in a second terminal. Restart with the applied graph when an older revision is active.

For a ready managed MuJoCo child, a routine IsOS Stop or Restart uses EOS’s managed shutdown fence. A confirmed selected receipt resolves only that temporary fence after dual actuator stop, child reap, and cleanup; it does not create or clear a genuine `safety.estop` latch or a prior robot inhibit. An unknown receipt leaves movement blocked. Check the selected IsOS terminal outcome and the next run’s planner-visible binding before issuing a new movement. A prior estop or uncertain movement still needs its separate owner recovery; never clear `.control` or replay the old command ID to make Restart pass. See the [managed shutdown contract](../interfaces/simulator-stop.md#routine-shutdown-state-distinct-from-emergency-stop).

EOS reads the applied graph without modifying it. IsOS resolves the verified executable and scene; EOS launches MuJoCo and connects only the declared adapters. The R06 stop path fences admitted actuators before child termination and reports unknown when proof fails; the remaining live fault cases are recorded in the [stop design](../design/runtime/simulator-stop.md). A missing package, changed linked asset, invalid scene, or incompatible typed edge fails admission before hardware starts. The dispatcher remains the only command route for base and manipulator movement.

The [historical TOML graph](../../../examples/v2/mujoco-isos.toml) is a read-only `1.0.0` compatibility fixture. Its `scene = "default"` and `@v1.0` nodes are not a new `0.1.1` package deployment. Use the verified node set above for first use; migrate an existing graph only through IsOS's explicit revision-checked migration.

**R05 EOS first-use behavior:** EOS launches its verified child with `--managed-strict-socket` and no historical gRPC or benchmark listener. Before reporting ready, it probes every declared camera/base/arm device and requires `version:"2.0.0"`, the echoed request ID, `ok:true`, `accepted:true` and `features` containing `strict-socket-v1`. A socket file alone is startup progress. If a feature, device or camera render is unavailable, check the current run's first `eos_start_failed` and the backend node's status. After ready, inspect camera capture and base/arm observations. Managed movement primitives are exposed only after both sockets prove the same-generation `strict-stop-v1` terminal fence; readiness alone does not confirm a later physical stop. A later admitted movement with `OutcomeUnknown` must not be resent automatically.

If a selected graph declares base or arm and the companion stop socket is still starting, EOS waits for its strict proof before reporting ready. If the socket never becomes available by the startup deadline, inspect the current run's `eos_start_failed` and backend log; no movement was admitted. A camera-only graph can be read-only without a stop generation.

## Observe the camera

The preset's EOS camera gate is the declared source for bounded `VisionFrame` output from EOS `CameraFrame` data. When an IsosUI Vision panel is available, select that active compatible gate as its preview source; the preset does not supply a UI node binding. The browser receives JPEG frames over a read-only WebSocket and displays the delivered frame rate. The initial target rate is the camera's declared rate; Vision settings can bound FPS, image size, and JPEG quality. Any later UI-owned source binding must use the typed gate output and cannot bypass EOS command admission.

Use `isos status` to compare saved and active revisions and `isos logs eos` or the IsosUI log viewer to inspect startup and camera faults. Find the current run's first `eos_ready` or `eos_start_failed` by its `run_id`; `eos_ready` confirms local EOS readiness while IsOS status confirms deployment readiness. The Rust EOS producer writes bounded JSONL to stderr. IsOS owns the deployment log store under `data/logs/<deployment>/`; managed raw MuJoCo output stays in `owner/eos/mujoco.log` and is not a shared EOS event. A correlated stop uses the same `correlation_id` on `eos_stopping`, any `eos_stop_unconfirmed`, and `eos_stopped`. Use `isos deployment stop` to stop only the graph, or `isos stop` to close the full manager and workbench.

## R06 managed stop first use

**Implemented owner path; the original R06 row and scoped M3 exit are qualified.** Probe `aether-runtime --capabilities` for the exact `managed-stop-v1` feature. That token announces EOS contract support only; it is not a stop result. Before active movement, confirm that EOS discovered `strict-stop-v1` on both MuJoCo sockets before trying an active movement binding. Inspect the active IsOS revision and EOS capability availability, submit one bounded primitive with its command ID, and use the correlated operation result rather than resending an uncertain movement. MuJoCo's `motion.stop` is not advertised because its physical stop is whole-robot; use the robot-level `robot.stop` binding when available. For deployment Stop, use the workbench or run `isos deployment stop`; IsOS stops EGIA and other dependents before EOS, while EOS closes admission, obtains separate base and arm hold proof through MuJoCo's companion stop lane, then terminates, drains the child group and reaps its leader. Inspect the current `run_id` in IsOS process status: `terminal.outcome:"confirmed"` requires the selected EOS raw-pipe terminal receipt and clean exit; `unknown` means physical stop, descendant drain or cleanup could not be proved. `eos_stopped` alone is diagnostic, and a vanished PID is not confirmation. For an unknown stop, keep movement disabled and follow the owner recovery procedure before another Run or Restart. A GUI/render failure before ready is a startup failure; a blocked render after ready can still allow confirmed stop through the separate lane. Child crash, stop timeout, surviving descendant or forced kill yields unknown stop. Find the first event and stop correlation through log rotation, then migrate an old graph only through IsOS's explicit revision-checked migration after the deployment is stopped.

The [R06 qualification record](../design/runtime/simulator-stop.md#merged-owner-and-scoped-qualification-status-2026-11-02) pins reviewed owner source and raw selected-pipe results. Two fresh headless IsOS scenes confirmed dual stop after governed base and arm movement. A missing stop socket, child/provider loss and forced kill returned unknown; a withheld stop response and forged receipt on a different child pipe also stayed unknown through manager restart and blocked Restart. A blocked camera capture may finish while shutdown seals new capture and publication; its worker result is bounded and cannot delay the companion stop lane. Those replays do not cover live physical partial proof, manager death during Stop, or post-ready GUI render block. The original R06 owner row and scoped M3 exit are now qualified by the [M3 record](../../island/m3-qualification.md); the broader live cases and M5 remain open.

## Benchmark isolation

The package's benchmark harness uses a separate direct-launch simulator outside the managed EOS child for seeded reset, stepping, ground-truth state and scoring. The managed strict child rejects that socket because Reset and Step would change its engine outside Dispatcher. That protocol is described in the [package repository](https://github.com/Island-Team/isos-mujoco-sim). EOS clients use graph-declared camera capture, observations and dispatcher-admitted movements; they do not receive benchmark state. Run the package's `ctest --test-dir build --output-on-failure` and the IsOS/EOS integration tests when changing the simulator or its node declarations.

**Related:** [IsOS package workflow](https://github.com/Island-Team/isos/blob/master/docs/isos/usage/packages.md) · [EOS hardware design](../design/runtime/hardware.md)
