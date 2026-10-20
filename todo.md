# EOS roadmap and runtime follow-ups

[中文](docs_cn/eos/chore/todo_cn.md)

This is a planning backlog, not a statement that the listed capabilities are implemented or committed to a release date. The current qualification baseline is native Ubuntu 22.04/24.04 on NVIDIA GPU PCs and Jetson systems. Keep the single Dispatcher release gate and deployment-owned hardware limits through every milestone.

## Model library discovery and hot reload

**Now:** Exact model IDs, capabilities, enabled states, endpoints, and adapters come from startup configuration. EOS calls separately started HTTP or vision WebSocket providers through bounded workers; catalog changes require a restart.

**Goal:** Discover and validate deployable model definitions, atomically publish a new catalog generation, and drain or reject in-flight calls before replacing a provider. Keep inference services external and fail closed on schema or capability mismatch.

**Depends on:** A versioned model-package contract, provenance records, generation ownership, and a rollback rule for unhealthy replacements.

**Done when:** Installing, replacing, disabling, and rolling back a model can be demonstrated without restarting `eosd`; concurrent callers see one valid generation, and invalid packages never become callable.

## Python skill authoring through pybind11

**Now:** Trusted native ABI v2 libraries load at startup. Managed and external skill runtimes are placeholders; EOS does not execute Python skills or reload native libraries.

**Goal:** Offer capability-scoped Python author APIs through pybind11 in an out-of-process skill host, including typed parameters/results, cancellation, signals, observations, and primitive calls through Dispatcher.

**Depends on:** The external host protocol, package admission, process supervision, and a stable Python SDK ABI/version policy.

**Done when:** A user-authored Python package can be installed, discovered, run, cancelled, and rejected for undeclared capabilities without embedding Python in `eosd`.

## Skill sandbox placeholder and isolation

**Now:** Built-in coroutine skills and native libraries are trusted and share the daemon address space. The external runtime folder is fail-closed and launches no subprocess.

**Goal:** Define the future sandbox seam for testing embodied skills with explicit resource, filesystem, network, device, time, and termination policy. Initial work is a contract and inert integration point, not a claim of containment.

**Depends on:** External skill-host supervision and capability-scoped IPC. Choose isolation technology only after threat-model and platform review.

**Done when:** The placeholder has a documented admission/lifecycle contract and negative tests proving no unconfigured sandbox or external skill can execute; later containment work has a measurable test matrix.

## IPC and shared-memory optimization

**Now:** Built-in skill IPC uses bounded, process-local JSON mailboxes. Simulator IPC uses a separate local socket. There is no general cross-runtime delivery or shared-memory transport.

**Goal:** Add a bounded external-host protocol and optional shared-memory transfer for large frames or tensors, preserving message identity, ownership, deadlines, cancellation, and privacy controls. Small control messages remain on a simple framed channel.

**Depends on:** External host lifecycle, payload lifetime rules, backpressure metrics, and benchmark traces from representative Jetson and PC workloads.

**Done when:** Correctness tests cover crash, timeout, stale handle, and backpressure cases; benchmarks show lower copies and latency without changing public skill semantics.

## Backend ecosystem and ROS 2

**Now:** One admitted binding owns a robot per process. Demo, host ALSA/V4L2, native `aplay`, and optional ROS 2 are present. ROS 2 support is selected at build time; site graph bridges and physical safety limits remain deployment-owned.

**Goal:** Qualify more standard ROS 2 device mappings and reusable adapter packages while keeping endpoint, frame, joint, and controller details in site bindings. Add clear compatibility and health evidence per adapter.

**Depends on:** Contract tests, representative ROS graphs, deployment-owned driver qualification.

**Done when:** A new backend can pass admission, feed, command, stop/estop, disconnect, and recovery tests without changing northbound contracts or bypassing Dispatcher.

## Dynamic graph and visual WebUI

**Now:** Hardware and model bindings are startup configuration; workflows are submitted as ActionLists. The simulator has a separate WebUI API. There is no visual graph editor joining models, skills, primitives, and devices.

**Goal:** Provide a versioned graph definition and WebUI for wiring model outputs, skill calls, primitive bindings, and hardware observations. Show types, units, dataflow, health, and rejected edges before activation.

**Depends on:** Stable catalog/discovery APIs, model generations, skill capabilities, and a graph validator that respects the Dispatcher gate.

**Done when:** An operator can compose, validate, save, load, and inspect a graph; invalid or unsafe edges are rejected, and visual execution state matches runtime events.

## End-to-end and multi-model inference

**Now:** EOS calls individual configured models through bounded workers; camera enrichments can run independently, but there is no unified multi-model scheduler or end-to-end model execution contract.

**Goal:** Support composed perception-to-action models and coordinated multi-model inference with explicit resource budgets, batching or placement policy, freshness, provenance, and safety checks at the existing command gate.

**Depends on:** Dynamic model catalog, graph types, measured GPU/CPU/memory budgets, and cancellation/backpressure behavior.

**Done when:** Representative chained and parallel model workloads meet published latency and memory budgets on a qualified PC and Jetson, with deterministic failure and no direct actuator bypass.

## Multi-OS support

**Now:** The supported build and deployment baseline is native Ubuntu 22.04/24.04 on NVIDIA GPU PCs and Jetson systems; optional ROS 2 variants follow their matching host distributions. Cross-compiling ARM64 from x86 is not supported.

**Goal:** Audit platform dependencies and select the next OS target from measured build, driver, ROS, IPC, packaging, and support costs. Then add a qualified build and deployment path for the chosen target.

**Depends on:** A portability inventory, target selection record, CI runner or representative hardware, and adapter capability matrix.

**Done when:** The target is selected with evidence, CI builds and tests its portable core, and deployment documentation states which hardware and optional adapters are qualified. No additional OS is promised before that selection.

## Cross-cutting runtime follow-ups

- Add durable skill execution recovery and replay across `eosd` restarts; diagnostic journald output is not recoverable manager state.
- Define restart-safe retry policy for unknown physical outcomes. Never implicitly retry an effect after an ambiguous release.
- Add preemptible isolation for callbacks that ignore cooperative cancellation; extend scheduling priority only when deployments require it.
- Specify catalog pagination, namespaces, compatibility ranges, deployment constraints, and verifiable package provenance/load history.
- Harden Dispatcher admission with device health, freshness, watchdog, and lease policy without creating a second release gate.
- Define authenticated Agent access, authorization, tenant isolation, and security-grade audit requirements for network deployments.
