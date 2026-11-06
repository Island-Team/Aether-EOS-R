# Shared interface catalog

[Island architecture](../architecture.md) · [Package contract](packages.md)

The unified IsOS source uses one `isos` graph core. The owner pages define precise fields, units, validation and compatibility; this catalog routes readers to them rather than creating a second schema.

| Boundary | Owner contract |
| --- | --- |
| Graph, packages, local API and process lifecycle | [IsOS interfaces](../../isos/interfaces/README.md) |
| Hardware, Dispatcher, typed payloads, gRPC and MuJoCo adapter | [EOS interfaces](../../eos/interfaces/README.md) |
| EI stages, safety and EOS link | [EI interfaces](../../ei/README.md) |
| Browser graph, logs, Vision and controls | [IsosUI interfaces](../../isosui/README.md) |
| Shared package composition | [Package contract](packages.md) |

New graph `0.2.0` type IDs use `isos.<layer>.<name>@v0.2`; product source is `0.2.1`. HTTP, protobuf and external package versions remain independent. Historical interface records retain their original published identities.
