# IsOS layer connections and data flow

[中文](../../docs_cn/island/layer-dataflow_cn.md) · [Architecture](architecture.md) · [Unification design](unification-v0.2.1.md)

**0.2.1 integration target, unreleased.** One `isos` graph core contains manager layers, `eos_*` execution layers, `ei_*` cognition layers and the `ui` browser layer. A path expresses placement. An input source expresses a typed data edge. A reference or process dependency expresses ordering. None of these grants command authority or replaces an explicit cross-process transport.

| Step | Boundary and owner | Evidence |
| --- | --- | --- |
| Install | IsOS verifies exact package receipts and assets; the external MuJoCo package is not part of this repository. | Pin, digest, availability or precise failure. |
| Compose | IsosUI edits the registered graph; IsOS validates type placement, ports, selectors and edges. | Selected catalog, descriptions and path-specific diagnostics. |
| Apply | IsOS compares the saved revision and writes immutable, revision-bound EOS/EI projections. | Distinct saved and active revisions. |
| Run | IsOS starts supervised EOS and EI processes; EOS starts its verified MuJoCo child and adapters. | Readiness reported by each owner. |
| Observe and act | EOS produces camera and hardware evidence. EI reaches EOS through typed gRPC. The browser reaches EOS through its loopback HTTP/WS bridge. Both control paths enter the EOS Dispatcher. | Provenance, target health, command ID and exact result. |
| Stop | IsOS closes the deployment; EOS stops admitted actuators before terminating MuJoCo; IsOS reaps dependent children. | Confirmed or explicitly unknown outcome. |

Cross-process data requires a declared transport even though both nodes live under `isos`. EI safety can refuse a proposal but cannot bypass EOS admission. A graph wire declares a possible data route; it does not execute a command. IsosUI does not send a command by drawing or editing an edge. The [EOS hardware interface](../eos/interfaces/hardware.md), [dispatcher design](../eos/design/runtime/dispatcher.md) and [EI southbound contract](../ei/southbound.md) own precise payload semantics.

Supported `0.1.0` graphs are inspected read-only until IsOS explicitly migrates paths, types, edges and bindings atomically. The historical qualification pages remain evidence for the commits and versions they name; they do not certify this new single-core path. [First-use replay](q01-first-use-design.md) and [implementation milestones](implementation-milestones.md) record the required integrated verification.
