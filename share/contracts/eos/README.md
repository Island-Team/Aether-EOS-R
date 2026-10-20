# EOS contract source

`gateway.proto`, `skills.proto`, and `workflow.proto` define the gRPC services used by other processes. The `actions/`, `body/`, `world/`, `system/`, `robot/`, `perception/`, `workflow/`, and `skills/` folders contain their domain messages and schema sources. Source paths are `eos/...`; the public wire package remains `island.eos.v1`.

`options/` holds shared schema annotations. `external/` holds the HTTP adapter's route annotations and private model and simulator contracts. The [interface guide](../../../docs/eos/develop/interfaces/README.md) mirrors the domain folders and documents gRPC, model, simulator, skill, and hardware boundaries. The authoritative hardware interface is the native [`<eos/hardware/backend.h>`](../../../src/eos/include/eos/hardware/backend.h), with a [southbound ownership note](southbound/README.md).

Generated public JSON schemas stay under `share/ext/eos/`, and HTTP OpenAPI stays under `share/ext/north_http/`.
