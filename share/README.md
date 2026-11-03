# Shared resources

Resources are grouped by owner. EOS owns the public wire contracts and runtime
schemas under [`eos/`](eos/contracts/eos/README.md). An extension owns its
private artifacts under `ext/<name>/`; those files do not become EOS contracts
merely because EOS can launch that extension.

| Directory | Contents |
| --- | --- |
| [`eos/contracts/`](eos/contracts/eos/README.md) | EOS Protobuf source, including versioned interfaces used by external peers |
| [`eos/schemas/eos/`](eos/schemas/eos/) | Generated public runtime JSON Schemas and manifest |
| [`eos/workflows/`](eos/workflows/README.md) | Local deployment workflow templates; JSON files are ignored by Git |
| [`eos/skills/`](eos/skills/README.md) | First-party skill source and authoring placeholders |
| [`ext/north-http/openapi/`](ext/north-http/openapi/north-http/openapi.json) | Generated HTTP adapter description |

Build and install rules preserve this ownership split. Teaching examples live
under [`examples/`](../examples/README.md).
