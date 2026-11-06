# IsOS

IsOS is the single deployment graph core for the manager, EI cognition, EOS execution, and the IsosUI workbench. It stores the graph, verifies exact package pins, serves a local workbench API, and supervises its direct children. A package declares registered node types, resources, and processes through `node_contributions`. IsOS verifies the declaration and payload bytes; EI and EOS validate their own process projections and lifecycle semantics. See the [IsOS guide](../../docs/isos/README.md) for the reader-first documentation.

## First use

```sh
isos init --mode dev
isos status --json
isos logs all --current
```

Install or link a release that publishes `isos.package@v1`, select its registered node type and declared resource IDs in the graph, then Validate, Apply against the current revision, and Run. The [package guide](../../docs/isos/usage/packages.md) and [run guide](../../docs/isos/usage/run.md) give the sequence. New graphs use format `0.2.0` and only the `isos` core, with `eos_*`, `ei_*`, and `ui` layers. Migration from supported older graphs is previewable; its Apply remains closed until the full owner and launch checks pass. If the selected owner or adapter is unavailable, IsOS rejects admission without replacing the active run.

`isos serve` hosts the local workbench; `isos open` starts or opens it. [Configure IsOS](../../docs/isos/usage/configuration.md) to select process binaries and the verified UI bundle: `config/isos.example.toml` supplies defaults and `config/isos.toml` supplies local overrides. `ISOS_HOME` selects an isolated `config/`, `data/`, and `run/` home for testing. The API route version remains `/api/isos/v1/*`; migration preview uses its separately versioned `schema_version: "0.2.0"`.

The [live interfaces](../../docs/isos/interfaces/README.md), [type catalog](../../docs/isos/layers/README.md), [implemented design](../../docs/isos/design/README.md), and [verification](../../docs/isos/maintenance/verification.md) contain the remaining details. [中文](README_cn.md)
