# EOS contract source

`gateway.proto`, `skills.proto`, and `workflow.proto` define the existing `island.eos.v1` gRPC services. `capabilities.proto` adds the `island.eos.v2` agent-facing capability and operation service. The `actions/`, `body/`, `world/`, `system/`, `robot/`, `perception/`, `workflow/`, and `skills/` folders contain their domain messages and schema sources. Source paths are `eos/...`; contract packages carry their own wire version.

`options/` holds shared schema annotations. `external/` holds the HTTP adapter's route annotations and private model and simulator contracts. The [interface guide](../../../docs/eos/interfaces/README.md) mirrors the domain folders and documents gRPC, model, simulator, skill, and hardware boundaries. The authoritative hardware interface is the native [`<eos/hardware/backend.h>`](../../../adapters/eos-cxx/include/eos/hardware/backend.h), with a [southbound ownership note](southbound/README.md).

Generated public JSON schemas stay under `share/eos/schemas/eos/`, and HTTP OpenAPI stays under `share/ext/north-http/openapi/north-http/`.
