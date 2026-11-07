# EGIA configuration adapter for IsOS

[中文](../../../docs_cn/ei/interfaces/isos-config.md)

`bin/egia-config` is a read-only, side-effect-free CLI boundary. It imports EGIA's configuration models but never starts the decision pipeline, connects to EOS, binds a listener, or writes files. IsOS owns the configuration server and writes the returned files atomically.

The `setup` operation is the v1 on-demand boundary. IsOS sends one JSON object on stdin: `{ "schema_version": 1, "op": "catalog|validate|render", "core": "egia", "revision": "<saved graph revision>", "graph": <full 0.1.0 or legacy 1.0.0 graph>, "endpoints": {"egia_northbound": {"host": "127.0.0.1", "port": 16060}, "eos_gateway": {"transport": "tcp", "host": "127.0.0.1", "port": 16022}}, "packages": [] }`. The request is limited to 1 MiB and 256 resolved packages. It returns `{ "schema_version": 1, "ok": true, "diagnostics": [], "files": [...] }`; `catalog` also returns the existing profile and defaults under `catalog`. `validate` returns no files, and `render` returns the three legacy overlays and, for a complete new stage graph, a revision-bound `egia/stages.json` overlay. IsOS alone writes these files beneath its managed config root. The EGIA core of the saved graph must exist; IsOS owns graph revision checks and package verification.

To inspect the configured binary before loading a graph, IsOS sends `{ "schema_version": 1, "op": "identity", "core": "egia" }` to `egia-config setup --json`. No graph, revision, endpoints, or packages are accepted for this operation. A successful reply contains `identity: { "core": "egia", "product_version": "0.1.0", "graph_version": "1.0.0", "node_type_version": "1.0", "supported_graph_versions": ["0.1.0", "1.0.0"], "supported_node_type_versions": ["0.1", "1.0"], "setup_schema_version": 1 }` alongside the usual `schema_version`, `ok`, empty `diagnostics`, and empty `files`. `product_version` is the EGIA package version; the remaining versions identify the supported IsOS graph, node type, and setup interface. IsOS uses these values to report compatibility and launch readiness. Invalid identity input exits 2 with a diagnostic and no files.

`eos_gateway` must identify a verified EOS **gRPC** endpoint, either TCP with `host` and `port` or UDS with an absolute `path`. The EOS HTTP control listener at `16020` and catalog at `16021` are not gRPC substitutes. When no gateway is available, `catalog` still works, while `validate` and `render` return a diagnostic and exit 2. This keeps EGIA runtime enablement gated until a compatible EOS gateway is configured.

## Current single-core setup reader (0.2.1 source)

IsOS sends setup schema `2` to `ei/bin/ei-config setup --json`, with `core:"egia"` as the compatibility owner role. For graph `0.2.0`, `graph` is a read-only projection of the sole `isos` core containing only `ei_*` layers; `revision` is the saved source graph's lowercase SHA-256. Exact stage IDs use `isos.ei_<layer>.<name>@v0.2`. The identity reply reports product `0.2.1`, graph `0.2.0`, node type `0.2`, and legacy readable versions. Catalog returns the new stage IDs for a `0.2.0` projection. Validate and render check placement, settings, local edges, and EOS gateway equality without changing the source graph. Rendered `ei/stages.json` preserves the exact 0.2.0 EI owner projection and supplied revision; the EI process adapts it in memory for its stage runtime. The other managed overlays are also written under `ei/`. The existing `egia-config` command and graph `0.1.0`/`1.0.0` readers remain available. The older setup examples and dated evidence below document those historical interfaces.

## Operations

Run `egia-config describe|defaults|profiles|render|validate --json`. Every operation prints one JSON object with `schema_version: 1`, `ok`, `data`, and `diagnostics`. A diagnostic has `path`, `code`, and `message`; invalid input exits with status 2. `describe` reports EGIA's product version and required EOS protocol services. `profiles` returns the `egia.agent` profile at version `1.0.0`, with one typed EOS gRPC input and its required contracts. Profile version and product version are independent.

`render` and `validate` read one JSON object from standard input:

The profile also exposes ordered `settings_fields` for graph editors. Each descriptor uses a dotted `name` relative to `values`, a `type`, `label`, and `description`, plus applicable `default`, `required`, `min`, `max`, `exclusive_min`, or `options` constraints. The order is listener host, listener port, status frequency, EOS transport, EOS host, EOS gRPC port, and EOS socket path. For example, the listener port descriptor is `{"name":"server.port","type":"integer","label":"Listener port","description":"Port for the EGIA northbound WebSocket listener.","default":16060,"min":1,"max":65535,"required":true}`. `gateway.host` and `gateway.grpc_port` apply to TCP; `gateway.path` applies to UDS. IsOS derives the UDS path from the connected EOS node, so it should show that value without accepting conflicting edits. The `defaults` operation returns a nested object with `server` and `gateway` objects, matching the `values` shape below. The older `fields` map remains for existing consumers.

```json
{
  "schema_version": 1,
  "values": {
    "mode": "isos",
    "server": {"host": "127.0.0.1", "port": 16060},
    "gateway": {"transport": "uds", "path": "/run/user/1000/isos/deployments/main/eos.sock"}
  }
}
```

`validate` checks editable values and returns their normalized form. `render` returns `data.files` entries with relative `path` and `content`: `egia/egia.toml`, `egia/modes.toml`, and `egia/profiles/isos.toml`. The paths are relative to IsOS's managed configuration root. IsOS must seed the rest of the EGIA configuration, including `prompts.toml`, from its installed/source `etc/egia` before applying these overlays. IsOS launches EGIA with that root as `EGIA_ETC` and `--mode isos --managed --no-tui`. All generated paths stay inside the managed root.

The TCP gateway form is `{"transport":"tcp","host":"127.0.0.1","grpc_port":16200}`. Existing profiles without `transport` remain TCP. The UDS form requires an absolute `path`, rejects a TCP host, and gives gRPC the `unix:///absolute/path` target directly. IsOS creates the private socket parent and starts EOS before EGIA; EGIA never proxies through IsOS. The runtime listener defaults to loopback port `16060`.

## Port-default verification

**Q01 owner binding:** IsOS selects one private EOS Vision endpoint and writes the separate [read-only Vision binding](vision-evidence.md) as `egia/vision-binding.json` plus an owner grant file. This binding remains outside the graph and the schema-1 `egia-config setup` response. The EGIA managed runtime now validates its deployment run, graph revision, gate, source, render and grant independently of the existing gRPC `eos_gateway`. The Vision endpoint cannot replace the gRPC gateway or assume fixed `16020`/`16021` ports. Selected IsOS integration and Q01 live replay remain separate gates.

The EGIA listener is `127.0.0.1:16060`; its IsOS profile, standalone config, TUI, and optional sandbox media route use the same default. The EOS southbound endpoint remains an independent connection (`16003` in older standalone examples, or a verified IsOS-resolved gRPC endpoint). The optional sandbox EOS gRPC gateway defaults to `16022`; an explicit configured port is preserved. EGIA's listener does not use the IsOS `16000`/`16001` or EOS `16020`/`16021` ports.

Run `uv run ruff check .`, `uv run pytest`, `uv run pyright`, `PYTHONPATH=src:libexec uv run lint-imports`, and `./bin/islandctl validate` after changing this adapter. The 2026-10-31 first-public-contract run passed all five gates (806 tests); complete output is under `logs/test/20261031-egia-public-v1/`. EOS-owned gRPC service names and their versions remain independent of the IsOS setup and graph versions.

## Compatible 0.x graph reader

The [P05 reader-feature contract](reader-features.md) extends this G05 identity with exact `supported_node_types` and `features`, and adds a selected `egia --capabilities` runtime probe. These EGIA fields and the no-start runtime probe are implemented; IsOS admission and EOS Gateway readiness remain separate gates. A graph envelope version alone does not make a managed EGIA launch ready.

The schema 1 `setup` envelope, operations, and legacy identity scalars remain. Identity also reports `supported_graph_versions: ["0.1.0", "1.0.0"]` and `supported_node_type_versions: ["0.1", "1.0"]`. The `graph_version: "1.0.0"`, `node_type_version: "1.0"`, and `setup_schema_version: 1` scalars remain for old callers. A version array is authoritative when present; an old adapter may omit it. The graph-version array describes accepted **envelopes**, not permission to use every node identity in that envelope.

For both graph envelopes, `catalog`, `validate`, and `render` preserve the four registered legacy EGIA design identities: `egia.common.source@v1.0` and `egia.common.context@v1.0` at `egia/common/data`, and `egia.common.planner@v1.0` and `egia.common.guard@v1.0` at `egia/common/process`. They do not grant a runtime route or turn those design nodes into implemented EGIA stages. The [seven exact `@v0.1` stage types](pipeline.md) require a complete graph, nonblank instance descriptions, exact typed edges, and a link gateway matching the IsOS-verified endpoint. Their managed render adds `egia/stages.json`; the current IsOS reader still needs a separate type-catalog update before Apply/Run. `catalog` supplies the existing `egia.agent` profile, defaults, `stage_types` descriptors, and `data_types` object schemas. Each stage port declares its global `egia.stage.<kind>@v0.1` data type, `in_process` transport, data class, and fixed-route status; inputs declare cardinality and graph-required status. IsOS checks the actual saved graph and node IDs before Apply or launch; it owns graph revision checks, migration, and saved bytes. EGIA's reader does not write or normalize graph files.

`catalog` checks the request envelope and exact EGIA type IDs and placements, then returns owner descriptors without composing graph edges. This lets IsOS report precise source-edge diagnostics during its own admission pass. EGIA `validate` and `render` still enforce complete stage composition, settings, fixed routes, and the resolved gateway.

| Identity field | Type / unit | Meaning and validation |
|---|---|---|
| `supported_graph_versions` | string array / semantic versions | Exactly `0.1.0` and `1.0.0` for this reader. Reject other envelope versions at `/graph/version`. |
| `supported_node_type_versions` | string array / node ID suffix versions | `0.1` stages and legacy `1.0` design IDs. Exact registered identity and placement are required; suffix alone grants no route. |

An unknown graph version returns `ok:false`, `files:[]`, exit 2, and `{"path":"/graph/version","code":"unsupported_graph_version","message":"EGIA supports graph 0.1.0 and legacy 1.0.0"}`. An unknown or unimplemented node type returns `unsupported_node_type_version` at `/graph/cores/egia/layers/<layer>/groups/<group>/nodes/<node>/node_type`; a known type at the wrong group returns `invalid_node_placement` at that path. JSON Pointer segments escape `~` and `/`. The missing `eos_gateway` remains a field-level error. IsOS verifies package manifests and executable paths.

For managed configuration, IsOS first copies complete installed EGIA defaults, including `models.toml`, `prompts.toml`, and `safety/gate.toml`, into a private revision directory, then applies the adapter's validated overlays atomically. These files remain required at EGIA startup. `render` still returns relative paths and content only; no secret values or arbitrary absolute path are accepted. IsOS passes the immutable revision directory as `EGIA_ETC` and starts EGIA only after a compatible EOS gRPC gateway is ready. A listener opening before mandatory discovery completes is not a ready signal.

In a system installation with prefix `<prefix>`, EGIA's factory fallback is exactly `<prefix>/share/defaults/egia` (directory of TOML files). IsOS selects and inventories this same directory before Apply and rechecks it before launch. A missing directory fails configuration loading; EGIA does not fall back to the resource copy under `<prefix>/share/island/defaults/egia`. Source and portable layouts continue to use their selected shared root's `defaults/egia` directory.

The 2026-11-04 installed-layout run used `uv run ruff format --check .`, `uv run ruff check .`, `uv run pytest -q` (940 passed), `uv run pyright` (zero errors), `PYTHONPATH=src:libexec uv run lint-imports`, and `./bin/islandctl validate`. A fresh prefix wheel/CMake install then loaded configuration through the system fallback and passed `egia-config` identity, `egia --capabilities`, and `islandctl validate`. The staged install test compares factory bytes and rejects a missing factory directory. Full local output is under `logs/test/20261104-installed-defaults/`.

The 2026-11-01 reader run used `uv sync --extra dev --extra vec`, `uv run ruff format --check src/egia/config_cli.py tests/egia/test_config_cli.py`, `uv run ruff check .`, `uv run pytest tests/egia/test_config_cli.py` (18 passed), `uv run pytest` (810 passed), `uv run pyright` (zero errors), `PYTHONPATH=src:libexec uv run lint-imports` (six contracts kept), and `./bin/islandctl validate`; all passed. The English and Chinese `eos-v2.md` guides were restored from the paired EGIA source; their JSON examples are covered by the full suite and their relative links were checked. Complete command output is in local ignored `logs/test/20261101-egia-reader-compat/`.
