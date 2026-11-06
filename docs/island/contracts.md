# Shared IsOS contracts

[中文](../../docs_cn/island/contracts.md) · [Interface catalog](interfaces/README.md) · [EOS wire ownership](contracts_aether.md)

The unified repository keeps one authored source for each public boundary. EOS owns protobuf and typed device payload source under `share/eos/contracts/eos/` and generated review artifacts under `share/eos/schemas/eos/`. EI in `ei/` generates its Python EOS transport bindings from this in-repository source; it does not keep a second authored EOS schema. IsOS owns graph `0.2.0`, selected type catalog, package receipts and revision-bound EOS/EI projections. IsosUI consumes those owner contracts and never creates a second actuator route.

New graph node IDs use `isos.<layer>.<name>@v0.2` under the one `isos` core. This graph identity does not rename published `island.eos.v1`, `island.eos.v2`, historical `egia.contracts.*` schema IDs, HTTP routes, or the external MuJoCo package/socket contract. EOS Dispatcher validates every actuator command; EI safety can refuse proposals but cannot bypass it. Cross-process edges need an explicit transport even when their nodes share the `isos` graph core.

Contract changes update the owner interface, generated bindings or schemas, bounded success and failure tests, and English/Chinese documentation together. The [EOS interface catalog](../eos/interfaces/README.md), [EI contract guide](../ei/contracts.md), [IsOS interfaces](../isos/interfaces/README.md) and [UI interfaces](../isosui/README.md) define exact fields and validation. Historical source pins and qualification claims remain in their dated evidence pages; source version `0.2.1` is not a published release.
