# Run EOS: first use

[Docs](../../README.md) → [EOS](../README.md) → **Use EOS**. See the [architecture](../../island/architecture.md), [interface index](../interfaces/README.md), and [build details](../develop/build-and-run.md).

This path follows one deployment in the order an operator encounters it. Current IsOS authors `0.1.0` graphs with registered `@v0.1` types; EOS also reads known safe `1.0.0` graphs and their legacy types. Package selectors and the admitted camera-to-gate routed evidence edge are implemented. IsOS owns graph admission, packages and deployment; EOS owns admitted devices and commands.

## 1. Build

Install dependencies from the [quick start](../../../README.md#development), then run `./build.sh` in this checkout. Build IsOS and IsosUI in their checkouts. The external `island/mujoco-sim@0.1.1` package is installed through IsOS. See [ARM64 setup](arm64.md) when applicable.

## 2. Install the package and choose nodes

The current IsOS configuration file is `config/isos.toml` by default; `ISOS_CONFIG` may select another file. Set its EOS binary and UI bundle paths. Follow the [IsOS first-use guide](../../isos/usage/README.md) for registry installation and exact package pins. Installing a release exposes verified node types and assets; it does not start them.

Select a MuJoCo scene and its camera, base or manipulator nodes in an authored graph. IsOS resolves package and scene IDs to verified paths. The [MuJoCo setup](mujoco.md) covers the current path; the [node contribution contract](../interfaces/node-contributions.md) defines selector metadata. Independent model services have separate IsOS service configuration and shared status; they are outside this deployment graph.

## 3. Validate and apply

```sh
isos config validate <graph-file>
isos config apply <graph-file>
isos version check
```

Validation reports schema, node, package and route errors before activation. Revision-checked Apply publishes generated EOS/EGIA configuration for the selected graph. EOS reads its applied projection without changing the saved graph. See the [IsOS graph contract](../../island/retired-repository-references.md#ref-5226626f10b2) and [EOS hardware contract](../interfaces/hardware.md). Preflight compares advertised supported versions with the selected graph and exact package pins.

## 4. Launch and confirm readiness

Run `isos serve` and `isos run` in separate terminals; the latter remains attached. Use `isos status --json` to inspect deployment and service state. IsOS starts EOS; EOS admits nodes, starts MuJoCo as its direct child, connects to its owner-only socket and reports readiness. Failure is reflected in state and logs. EOS stops admitted actuators before terminating MuJoCo. See the [foundation lifecycle](../design/runtime/foundation.md).

## 5. Inspect and control

To inspect the implemented R01 camera evidence route, [inspect EOS evidence routes](routed-edges.md) after launch; the page distinguishes configured graph edges from live delivery.

Open IsosUI for graph inspection, edge priority/frequency, floating monitoring panels and declared primitive controls. For the Unix CLI, run `./bin/eos-cli --socket <runtime-socket>` using the applied deployment's socket. `discover` lists admitted nodes; `health <path>` and `capture <path>` inspect one; `quit` leaves the deployment running. See [CLI commands](repl.md) and [control](../interfaces/control.md). Every admitted actuator command passes through the EOS dispatcher.

## 6. Read logs and debug

First use `isos status --json` to identify the current deployment, then `isos logs all --lines 200` to inspect its recent ready or failure lines. `isos logs eos --lines 200` narrows output to EOS. In the current [Rust JSONL contract](../interfaces/events.md), an Apply or package failure before launch belongs to the IsOS manager-session ID; after launch, use the deployment run ID before source, node or correlation filters. IsOS provides cursor reads and rotation recovery so the first event remains findable. Follow one correlation ID across IsOS, EOS, MuJoCo and EGIA, then inspect authoritative status and graph revision. The [scheduler design](../design/runtime/scheduler.md) describes implemented edge throttle, overflow and stop events.

## 7. Stop, restart or upgrade

`isos deployment stop` stops the applied run; `isos restart` starts the selected revision; `isos stop` also shuts down the manager and its services. Before upgrading, review the exact package release and graph compatibility, migrate explicitly if needed, then validate and apply a new revision. Historical `1.0.0` graph bytes and `@v1.0` identities stay unchanged until revision-checked migration. See [packages](packages.md) and the [implementation TODO](../maintenance/todo.md).
