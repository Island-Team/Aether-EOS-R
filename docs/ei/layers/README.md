# EGIA node catalog

[中文](../../../docs_cn/ei/layers/README.md) · [Stage interface](../interfaces/pipeline.md) · [Architecture](../design/architecture.md)

Read this catalog after first use to locate a node by `core → layer → group → node`. The leaf page describes purpose, ports, lifecycle and failure behavior; the single [stage interface](../interfaces/pipeline.md) owns serialized and typed field definitions. All seven `@v0.1` kinds below have an A01 EGIA-side reader and managed owner binding. The stubbed link → perception → context → decision evidence path runs through typed ports. IsOS still needs its exact reader/catalog update before Apply/Run can select these graph instances. There is no WorkflowService, ActionList or workflow DAG node in the 0.x target. Missions and plans are the current agent state model. Known historical design IDs are `egia.common.source@v1.0`, `egia.common.context@v1.0`, `egia.common.planner@v1.0`, and `egia.common.guard@v1.0`; they are not aliases for the new stages and gain no execution route by being read.

| Core | Layer | Group | Node kind | Type ID |
| --- | --- | --- | --- | --- |
| `egia` | `io` | `link` | [gateway](io/groups/link/nodes/gateway.md) | `egia.link.gateway@v0.1` |
| `egia` | `evidence` | `perception` | [admission](evidence/groups/perception/nodes/admission.md) | `egia.perception.admission@v0.1` |
| `egia` | `cognition` | `context` | [builder](cognition/groups/context/nodes/builder.md) | `egia.context.builder@v0.1` |
| `egia` | `cognition` | `missions` | [queue](cognition/groups/missions/nodes/queue.md) | `egia.missions.queue@v0.1` |
| `egia` | `cognition` | `decision` | [planner](cognition/groups/decision/nodes/planner.md) | `egia.decision.planner@v0.1` |
| `egia` | `governance` | `safety` | [gate](governance/groups/safety/nodes/gate.md) | `egia.safety.gate@v0.1` |
| `egia` | `governance` | `monitor` | [health](governance/groups/monitor/nodes/health.md) | `egia.monitor.health@v0.1` |

The registered type description explains behavior and effects; each graph instance also carries its operator description. Graph references, source-edge scheduling, package pins and revision checks are IsOS-owned. The fixed safety-to-link and monitor emergency routes cannot be replaced by authored edges. Module algorithms remain under [`core/`](../core/modules.md), while the catalog is the operator-facing route to each node kind.
