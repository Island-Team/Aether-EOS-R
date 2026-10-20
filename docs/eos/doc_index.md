# Documentation index

One-line map of EOS documentation in this repository. Product-level layout and conventions live in the sibling Island documentation tree.

[中文](../../docs_cn/eos/doc_index_cn.md)

[Use EOS](usage/README.md) · [Develop with EOS](develop/README.md) · [Design](design/README.md) · [Release](chore/release.md) · [Aether contracts](../island/contracts_aether.md)

## Complete map

- [README.md](README.md): component overview, build, launch, installation, and selected references.
- [usage/arm64.md](usage/arm64.md): native Linux ARM64 prerequisites, build, test, launch, and ROS 2 guidance.
- [design/eos-2607.md](design/eos-2607.md): technical direction, architecture, invariants, and roadmap.
- [eos-2607_cn.md](../../docs_cn/eos/design/eos-2607_cn.md): Chinese mirror of the technical direction.
- [../island/layout.md](../island/layout.md): standalone EOS repository roles and combined-product mapping.
- [../island/conventions.md](../island/conventions.md): cross-repository code, documentation, module, and Git workflow.
- [chore/release.md](chore/release.md): release checklist.
- [../../todo.md](../../todo.md): EOS roadmap and runtime follow-ups ([Chinese](../../docs_cn/eos/chore/todo_cn.md)).
- [design/runtime/README.md](design/runtime/README.md): integrated runtime data lifecycle and cross-module conditions.
- [design/runtime/actionlist.md](design/runtime/actionlist.md): ActionList ownership, DAG scheduling, facts, supersession, no-retry semantics, logging, and verification.
- [usage/debugging.md](usage/debugging.md): cross-process request, cursor, audit, logging, privacy, and verification runbook.
- [develop/ext/README.md](develop/ext/README.md): extension ownership, backend qualification, profile semantics, and authoring rules.
- [develop/ext/north_http/README.md](develop/ext/north_http/README.md): standalone north HTTP process composition, ownership, lifecycle, logging, and test map.
- [develop/ext/north_http/client.md](develop/ext/north_http/client.md): single shared EOS gRPC channel, fixed generated stub set, lifetime, and diagnostics.
- [develop/ext/north_http/server.md](develop/ext/north_http/server.md): HTTP/SSE server ownership, bounds, cancellation, logging, and tests.
- [develop/ext/north_http/config.md](develop/ext/north_http/config.md): strict north HTTP configuration, precedence, defaults, validation, and security boundary.
- [develop/ext/north_http/audit.md](develop/ext/north_http/audit.md): external non-authoritative ActionList projection, privacy allowlist, durable journal, recovery, health, logging, and tests.
- [develop/interfaces/README.md](develop/interfaces/README.md): northbound, canonical payload, and southbound contract map.
- [develop/interfaces/external/http.md](develop/interfaces/external/http.md): complete REST/SSE WebUI contract, CORS, errors, request IDs, security, and migration.
- [develop/interfaces/workflow.md](develop/interfaces/workflow.md): native gRPC ActionLists, events, cursor retention, atomic snapshots, errors, and recovery.
- [develop/ext/backends/demo.md](develop/ext/backends/demo.md#profiles-and-operation): complete deterministic demo primitive and feed envelope, including the host-capture variant.
- [develop/ext/backends/host.md](develop/ext/backends/host.md): native ALSA microphone and Video4Linux2 camera capture backends.
- [develop/ext/backends/ros2.md](develop/ext/backends/ros2.md): optional ROS 2 mapping, freshness and failure semantics, plus site-local profile guidance.
- [develop/interfaces/body.md](develop/interfaces/body.md): northbound robot and device perception.
- [develop/interfaces/world.md](develop/interfaces/world.md): northbound fused-world perception.
- [develop/interfaces/actions.md](develop/interfaces/actions.md): northbound primitive discovery, release, and acknowledgements.
- [develop/interfaces/skills.md](develop/interfaces/skills.md): SkillService catalog, process-like execution CRUD, SIGNALs, watches, errors, and decision-client flow.
- [interfaces/skills/](develop/interfaces/skills/native/README.md): native ABI v2 skill authoring, packaging, SIGNALs, claims, and capability calls.
- [develop/interfaces/external/models.md](develop/interfaces/external/models.md): model request and response interfaces used by EOS.
- [develop/interfaces/external/simulator.md](develop/interfaces/external/simulator.md): standalone simulator lifecycle, v2 storage, private IPC, and WebUI HTTP/WebSocket APIs.
- [develop/interfaces/contracts.md](develop/interfaces/contracts.md): authoritative links and navigation for every protobuf and JSON Schema contract.
- [develop/interfaces/southbound/body.md](develop/interfaces/southbound/body.md): hardware observations received by EOS.
- [develop/interfaces/southbound/primitives.md](develop/interfaces/southbound/primitives.md): primitive operations released to drivers.
- [develop/interfaces/southbound/world.md](develop/interfaces/southbound/world.md): hardware observations used for world perception.
- [design/algorithms.md](design/algorithms.md): algorithm and edge-compute direction.
- [usage/configuration.md](usage/configuration.md): configuration keys, precedence, and validation.
- [design/lib.md](design/lib.md): shared clocks, logging, paths, and slots.
- [develop/ext/backends/demo.md](develop/ext/backends/demo.md): demo HAL adapter, device mapping, simulation, and raster implementation ownership.
- [develop/ext/tools/repl.md](develop/ext/tools/repl.md): diagnostic REPL.
- [design/runtime/hardware.md](design/runtime/hardware.md): backend admission, generated identity, and the process hardware binding.
- [design/runtime/init.md](design/runtime/init.md): strict configuration, immutable schema ownership, and process composition.
- [design/runtime/perception.md](design/runtime/perception.md): read-only perception namespace, inputs, outputs, and ownership boundaries.
- [design/runtime/models.md](design/runtime/models.md): configured outbound perception-model clients, contracts, and workers.
- [design/runtime/dispatch.md](design/runtime/dispatch.md): validated primitive dispatch and stop behavior.
- [design/runtime/northbound.md](design/runtime/northbound.md): gRPC transport ownership, services, replay behavior, logging, and shutdown lifecycle.
- [design/runtime/northbound/workflow.md](design/runtime/northbound/workflow.md): WorkflowService protobuf conversion, event streaming, cursor errors, logging, and tests.
- [design/runtime/skills.md](design/runtime/skills.md): multi-runtime skill subsystem overview and module map.
- [design/runtime/skills/manager.md](design/runtime/skills/manager.md): SkillManager runtime registry, exact routing, scheduling, claims, events, retention, logging, and tests.
- [design/runtime/skills/native.md](design/runtime/skills/native.md): trusted native ABI v2 runtime admission, capabilities, execution, isolation, logging, and tests.
- [design/runtime/skills/authoring.md](design/runtime/skills/authoring.md): execution-profile-neutral skill authoring requirements and review checklist.
- [design/runtime/skills/data.md](design/runtime/skills/data.md): descriptors, execution snapshots, events, signals, and retained data ownership.
- [develop/interfaces/skills/ipc.md](develop/interfaces/skills/native/ipc.md): provider IPC framing and protocol boundary.
- [design/runtime/skills/observability.md](design/runtime/skills/observability.md): bounded logs, metrics, privacy, and diagnostic fields.
- [design/runtime/skills/process-model.md](design/runtime/skills/process-model.md): process isolation, lifecycle, and supervision model.
- [design/runtime/skills/managed.md](design/runtime/skills/managed.md): fail-closed managed-runtime placeholder boundary.
- [design/runtime/skills/external.md](design/runtime/skills/external.md): fail-closed external-runtime placeholder boundary.
- [../../src/eos/ext/decision/README.md](../../src/eos/ext/decision/README.md): in-tree external decision client, policy, and event runner.
