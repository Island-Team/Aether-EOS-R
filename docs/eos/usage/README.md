# Run EOS: first use

[Docs](../../README.md) → [EOS](../README.md) → **Use EOS**. See the [architecture](../../island/architecture.md), [interface index](../interfaces/README.md), and [build details](../develop/build-and-run.md).

This path follows one deployment in the order an operator encounters it. The current Rust reader accepts established `1.0.0` graphs and `@v1.0` types. New `0.1.0` graphs, `@v0.1` types, selectors and routed-wire scheduling are **target design**; use a current graph until the [implementation TODO](../maintenance/todo.md) is complete. IsOS owns graph admission, packages and deployment; EOS owns admitted devices and commands.

## 1. Build

Install dependencies from the [quick start](../../../README.md#build-eos), then run `./build.sh` in this checkout. Build IsOS and IsosUI in their checkouts. The external `island/mujoco-sim@0.1.1` package is installed through IsOS. See [ARM64 setup](arm64.md) when applicable.

## 2. Install the package and choose nodes

The current IsOS configuration file is `config/isos.toml` by default; `ISOS_CONFIG` may select another file. Set its EOS binary and UI bundle paths. Follow the [IsOS first-use guide](https://github.com/Island-Team/isos/blob/master/docs/isos/usage/get-started.md) for registry installation and exact package pins. Installing a release exposes verified node types and assets; it does not start them.

Select a MuJoCo scene and its camera, base or manipulator nodes in an authored graph. IsOS resolves package and scene IDs to verified paths. The [MuJoCo setup](mujoco.md) covers the current path; the [node contribution contract](../interfaces/node-contributions.md) defines target selector metadata. Independent model services have separate IsOS service configuration and shared status; they are outside this deployment graph.

## 3. Validate and apply

```sh
isos config validate <graph-file>
isos config apply <graph-file>
isos version check
```

Validation reports schema, node, package and route errors before activation. Current Apply writes generated configuration for the deployment; the target revision-checked Apply publishes immutable EOS/EGIA snapshots. EOS reads the applied graph without changing it. See the [IsOS graph contract](https://github.com/Island-Team/isos/blob/master/docs/isos/interfaces/node-graph.md) and [EOS hardware contract](../interfaces/hardware.md). The target preflight compares advertised supported versions with the selected graph and exact package pins.

## 4. Launch and confirm readiness

Run `isos serve` and `isos run` in separate terminals; the latter remains attached. Use `isos status --json` to inspect deployment and service state. IsOS starts EOS; EOS admits nodes, starts MuJoCo as its direct child, connects to its owner-only socket and reports readiness. Failure is reflected in state and logs. EOS stops admitted actuators before terminating MuJoCo. See the [foundation lifecycle](../design/runtime/foundation.md).

## 5. Inspect and control

For the target R01 evidence route, [inspect EOS evidence routes](routed-edges.md) after launch; the page distinguishes configured graph edges from live delivery.

Open IsosUI for graph inspection and monitoring. The current UI does not yet show edge priority/frequency or the target floating workspace. For the current Unix CLI, run `./bin/eos-cli --socket <runtime-socket>` using the applied deployment's socket. `discover` lists admitted nodes; `health <path>` and `capture <path>` inspect one; `quit` leaves the deployment running. See [CLI commands](repl.md) and [control](../interfaces/control.md). Every admitted actuator command passes through the EOS dispatcher.

## 6. Read logs and debug

First use `isos status --json` to identify the current deployment, then `isos logs all --lines 200` to inspect its recent ready or failure lines. `isos logs eos --lines 200` narrows today's output to EOS. In the **target** [Rust JSONL contract](../interfaces/events.md), an Apply or package failure before launch belongs to the IsOS manager-session ID; after launch, use the deployment run ID before source, node or correlation filters. IsOS P07 will add cursor reads and rotation recovery so the first event remains findable. Follow one correlation ID across IsOS, EOS, MuJoCo and EGIA, then inspect authoritative status and graph revision. The [scheduler design](../design/runtime/scheduler.md) defines future edge throttle, overflow and stop events.

## 7. Stop, restart or upgrade

`isos deployment stop` stops the applied run; `isos restart` starts the selected revision; `isos stop` also shuts down the manager and its services. Before upgrading, review the exact package release and graph compatibility, migrate explicitly if needed, then validate and apply a new revision. Historical `1.0.0` graph bytes and `@v1.0` identities stay unchanged until revision-checked migration. See [packages](packages.md) and the [implementation TODO](../maintenance/todo.md).
