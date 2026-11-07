# EI–EOS contract boundary

[中文](../../docs_cn/island/eos-contracts.md) · [Shared contracts](contracts.md) · [EI EOS interface](../ei/interfaces/eos.md)

EI (formerly EGIA) and EOS are domains of the one IsOS graph core, but their processes retain an explicit transport boundary. IsOS supplies revision-bound projections. EI obtains discovery and exact capability bindings through the published EOS gRPC services and sends typed operations through the EOS gateway. EOS validates the active graph, target, payload and safety limits, then sends admitted work through its Dispatcher and journal. EI never opens the MuJoCo socket or calls an adapter directly.

EOS owns the authored protobuf and JSON Schema source under `share/eos/`. EI's generated Python bindings in `ei/` are derived artifacts from that local source. Published `island.eos.v1` Gateway and `island.eos.v2` CapabilityService identities remain independent of graph `0.2.0` and source `0.2.1`. Historical `egia.contracts.*` schema IDs and older source pins remain compatibility/provenance records, not instructions to restore a sibling checkout.

A caller-generated operation ID is the durable idempotency key. EI follows a pending result until a terminal owner outcome; transport loss or `OUTCOME_UNKNOWN` does not authorize automatic replay. Cancellation uses the published operation boundary. For exact arguments, acknowledgement and failure codes, use the [EOS interface](../eos/interfaces/README.md) and [EI EOS contract](../ei/interfaces/eos.md). Contract changes regenerate bindings and test admission, rejection, cancellation and uncertain outcomes with both owners.
