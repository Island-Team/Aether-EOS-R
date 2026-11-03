# EOS v1 graph setup adapter

[中文](../../../docs_cn/eos/interfaces/setup-v1_cn.md) · [Node profile](hardware.md)

IsOS invokes the selected `aether-profile setup --json` executable before starting EOS. The adapter reads one JSON request from standard input, validates the shared v1 graph using EOS's registered node types, and writes one JSON response to standard output. It does not bind a listener, open hardware, start EOS, or write deployment files. IsOS owns package verification, graph revision checks, and atomic installation of returned files. The runtime catalog on port `16021` remains a separate read-only endpoint.

The request is limited to 1 MiB. IsOS passes package-contributed node type sidecars through `packages: [{id, version, path}]`, where `path` is the absolute verified package directory. For `validate` and `render`, EOS reads any `node-types.json` in each listed package, checks that it stays inside the package directory and identifies the same package, and registers its contributed types and adapter families before validating the graph. The direct CLI can also accept repeated `--node-types <verified-json>` arguments. `endpoints` is the IsOS-resolved endpoint map. `revision` is a nonempty ASCII string of at most 128 bytes.

```json
{"schema_version":1,"op":"render","core":"eos","revision":"af44f1","graph":{"version":"1.0.0","robot":{"id":"robot","name":"Robot"},"cores":{}},"endpoints":{},"packages":[]}
```

`validate` returns diagnostics and no files. `render` returns the validated canonical graph as `eos/graph.json`, a relative path under IsOS's managed configuration root. IsOS must reject unsafe paths and install the file only if the saved graph still has the request revision. The response is limited to 4 MiB, including the JSON-escaped file content.

```json
{"schema_version":1,"ok":true,"revision":"af44f1","diagnostics":[],"files":[{"path":"eos/graph.json","content":"{\"version\":\"1.0.0\",\"robot\":{\"id\":\"robot\",\"name\":\"Robot\"},\"cores\":{}}"}]}
```

`catalog` requires only `schema_version`, `op`, and `core`. It returns `catalog` with `types`, `data_types`, `adapter_families`, and `primitive_specs`; its `files` array is empty. Invalid requests and graphs return `ok: false` with a diagnostic containing `path`, `code`, and `message`, and no files. They still exit with status zero so IsOS can display the diagnostic. An I/O failure exits nonzero.

`identity` also requires only `schema_version`, `op`, and `core`. Its response has empty `diagnostics` and `files` plus `identity: {core, product_version, graph_version, node_type_version, setup_schema_version}`. `product_version` is the EOS binary's Cargo package version; IsOS uses this operation to report the configured core build without selecting a Git commit. Package-contributed adapter families in `catalog` have `loadable: true` and require an IsOS-verified library at runtime.

```json
{"schema_version":1,"ok":true,"diagnostics":[],"files":[],"identity":{"core":"eos","product_version":"0.2.0","graph_version":"1.0.0","node_type_version":"1.0","setup_schema_version":1}}
```

These versions identify the EOS product, shared graph, node type registry, and setup exchange separately. The EOS `/v2/eos` control routes, MuJoCo socket protocol, `island/mujoco-sim@0.1.1` package, and durable journal retain their own independent versions.

## G05 identity and P05 reader features

The G05 adapter currently returns `supported_graph_versions:["1.0.0","0.1.0"]` and `supported_node_type_versions:["1.0","0.1"]` while retaining legacy scalar `graph_version:"1.0.0"`, `node_type_version:"1.0"` and schema 1. An array is authoritative when present; a suffix is not permission to accept an arbitrary full ID. `catalog` without `--node-types` reports the base registry, while repeated `--node-types <verified-json>` CLI arguments add package contributions before the request is handled. The `packages` request array is used by `validate` and `render`, not the early `catalog` reply.

P05 adds `identity.features` and `identity.supported_node_types` to match the same selected `aether-profile --capabilities` file. [Reader features and identities](reader-features.md) owns the exact field types, built-in IDs, required features, legacy fallback and negative preflight cases. IsOS owns public supported-set diagnostics and checks actual selected binaries before launch. The setup adapter retains its current `ok:false`, `diagnostics:[{path,code,message}]`, `files:[]` failure envelope. Current `invalid_graph` and `unsupported_version` setup codes remain distinct from IsOS P05 preflight codes.
