# 0.x implementation roadmap

[中文](../../../docs_cn/eos/maintenance/todo_cn.md) · [Target architecture](../../island/architecture.md) · [Layer data flow](../../island/layer-dataflow.md) · [Detailed cross-repo TODO](../../island/implementation-todo.md) · [Implementation milestones](../../island/implementation-milestones.md) · [Runtime foundation](../design/runtime/foundation.md)

**Target plan, with partial implementation.** This page records the EOS 0.2.0, EGIA 0.1.0, IsOS 0.1.0, IsosUI 0.1.0, and MuJoCo 0.1.1 work. Individual owner pages distinguish implemented slices from planned behavior. Use focused `refactor(v2)/<topic>` branches and keep complete output in `logs/test/<run-id>/`.

This EOS summary follows milestones M1–M5. The [detailed cross-repository TODO](../../island/implementation-todo.md) assigns task IDs, dependencies and acceptance evidence for every boundary in the [layer connection map](../../island/layer-dataflow.md). The 0.x target has **no WorkflowService, ActionList DAG executor, or workflow templates**. Historical protobuf and old C++ documentation stay cataloged for compatibility and last-caller audit; no new workflow node or UI is added.

## 1. Graph and interface compatibility

- [ ] Make IsOS, EOS, EGIA setup, and IsosUI read safe `1.0.0` graphs and `@v1.0` types while writing `0.1.0` and `@v0.1`. Parse by version, normalize in memory, retain saved bytes and revision until explicit revision-checked migration, and reject unknown future versions. Show the old arbitrary-command IsOS process type only with migration guidance; never execute it.
- [ ] Add module-owned `NodeType` descriptors, nonblank descriptions for new types, a fallback for old sidecars, and type-driven setting description, unit, bounds, choices, and selector metadata. The Rust description field/fallback is implemented; full type metadata and editor support remain planned. Keep instance `description` and old profile behavior. Add optional fields/defaults, preserve protobuf field numbers and JSON names, and test old clients and fixtures. Validate edge type/unit before Apply and report the exact rejected edge; display EOS live health separately.
- [ ] Add optional nice-scale `priority` and positive finite `frequency_hz` to edges. Implement [bounded routed delivery](../design/runtime/scheduler.md), explicit overflow/gap counters, and declarative-edge status. Verify no edge bypasses Dispatcher.

## 2. IsOS package, configuration, logs, and preflight

- [ ] Resolve selector options from verified installed package descriptors, registered types, and the draft graph. Extend node-options with value, label, description, availability, and reason. Ship MuJoCo scene/device selection; defer arbitrary model-weight execution.
- [ ] Render immutable, revision-specific EOS and EGIA config in IsOS data and pass paths to children. Preserve XDG/`ISOS_HOME`, secret references, and strict package pins. Compare advertised supported graph/type versions and features with the actual graph before start.
- [ ] Align EOS tracing, EGIA events, MuJoCo child output, and IsOS aggregation to the [common JSONL envelope](../../island/conventions.v1.3.1.md#testing-and-observability). Record exact verified package provenance and prelaunch failures with a manager-session `run_id` and optional correlation ID. Add one startup/ready event, state-change events, redaction, bounded rotation, filters, and cursor recovery.

## 3. Runtime, agent, and workbench

- [ ] Add Rust EOS gRPC health, Gateway Discover/ListResources/GetStatus, and CapabilityService for the current Python EGIA client. Retain published wire identities and typed errors; route actuation through Dispatcher only.
- [ ] Expose a versioned EOS host skill ABI: EOS supplies bounded capability callbacks and lifecycle; a verified skill package exports a descriptor and one entry point. Unconfigured hosts, invalid packages, and undeclared capabilities fail before guest execution. Skills cannot hold dispatcher, hardware, graph-edit or workflow-manager handles. Preserve existing C ABI symbols for old packages under explicit compatibility policy.
- [ ] Expose EGIA Python link, perception, context, mission/decision, safety, and monitor stages as typed graph nodes with one state owner each. Keep model-like services outside the graph, `egia.tensor` deferred, and degraded readiness distinct from a bound TCP port.
- [ ] Keep MuJoCo in EOS's internal adapter registry while IsOS owns the external exact-version package. Verify executable, scene, assets, socket, adapter/device binding, command identity, and stop behavior at Apply and run. Stop admitted actuators before simulator termination and reap.
- [ ] Add searchable, registry-driven IsosUI selectors; keep missing current values visible. Show/edit priority and frequency on each wire with legend and accessible text, plus type/unit and exact validation reason separate from live health. Add a floating node info card with distinct type and instance descriptions. Add independently dockable/floating control, monitor, Vision, and log panels with saved layout and visible stop/failure state. The first control demo selects one active primitive binding and target, invokes `primitive_typed` through the existing browser bridge, and shows a correlated result.

## 4. Cleanup, qualification, and release

- [ ] After last-caller checks, remove unused `adapters/eos-cxx/` C++ runtime, old launchers, CMake branches, tests, examples, and guides. Retain active CXX mock, adapter C ABI, and EGIA-required EOS schemas. Rename remaining `v2` examples/scripts to topic names without changing wire IDs.
- [ ] Run format, lint, unit and integration tests for old/new graphs, package sidecars, selectors, version mismatch, QoS fairness/overload, logs, UI, EGIA degraded startup, simulator crash/restart, uncertain commands, and safe stop. Measure fixed-workload throughput, median, p99, and environment before performance claims.
- [ ] Qualify one clean-home first-use run: install → graph preview → Apply → launch → observe → control → logs → failure/recovery → stop → upgrade. Review bilingual usage against the commands, then review/squash-merge each topic PR. Publish 0.x tags only after qualification; keep historical 1.x tags immutable.
