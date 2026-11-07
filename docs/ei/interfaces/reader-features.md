# EGIA reader and managed-runtime features

[中文](../../../docs_cn/ei/interfaces/reader-features.md) · [IsOS setup](isos-config.md) · [Stage contract](pipeline.md) · [Cross-repository P05](../../island/implementation-todo.md)

**P05 EGIA owner contract.** The setup identity and no-start runtime probe are implemented in this checkout; IsOS admission remains a separate integration gate. IsOS owns the saved graph, exact revision, selected executable files, package pins, generated configuration and admission. EGIA reports what its selected setup adapter can read and what its selected Python runtime can launch. A capability reply is not evidence that EOS's gRPC Gateway is ready or that IsOS has admitted a new EGIA stage graph.

## Exact selected probes

IsOS invokes the selected `egia-config setup --json` with `{"schema_version":1,"op":"identity","core":"egia"}` on stdin. The existing schema-1 success envelope keeps empty `diagnostics` and `files`. Its `identity` gains `supported_node_types` and `features` while retaining `core`, `product_version`, legacy scalar aliases `graph_version:"1.0.0"` and `node_type_version:"1.0"`, `supported_graph_versions:["0.1.0","1.0.0"]`, `supported_node_type_versions:["0.1","1.0"]`, and `setup_schema_version:1`. The historical P05 set below remains supported; A01 also advertises the seven exact `@v0.1` identities in the [stage catalog](pipeline.md). The full identity array is sorted:

1. `egia.common.context@v1.0` at `egia/common/data`;
2. `egia.common.guard@v1.0` at `egia/common/process`;
3. `egia.common.planner@v1.0` at `egia/common/process`;
4. `egia.common.source@v1.0` at `egia/common/data`.
5. `egia.context.builder@v0.1` at `egia/cognition/context`;
6. `egia.decision.planner@v0.1` at `egia/cognition/decision`;
7. `egia.link.gateway@v0.1` at `egia/io/link`;
8. `egia.missions.queue@v0.1` at `egia/cognition/missions`;
9. `egia.monitor.health@v0.1` at `egia/governance/monitor`;
10. `egia.perception.admission@v0.1` at `egia/evidence/perception`;
11. `egia.safety.gate@v0.1` at `egia/governance/safety`.

The setup feature array is `["setup-v1","catalog","graph-0.1-reader","egia-evidence-routing-v1"]`. `setup-v1` means the schema-1 identity, catalog, Validate and Render exchange exists; `catalog` means the existing profile/default catalog is available; `graph-0.1-reader` means the adapter can inspect the `0.1.0` envelope and the exact IDs it advertises. `egia-evidence-routing-v1` means Validate/Render compile the two exact eligible evidence routes in `catalog.routed_edges`, each with `source_type`, `output`, `target_type`, `input`, and `controls: ["priority","frequency_hz"]`. A01 can render a complete seven-stage graph, while the four historical design nodes remain non-executing. IsOS needs its separate reader/catalog update before admitting the new IDs and routes. EGIA setup continues to return its existing `{path,code,message}` diagnostics and no files on rejection. Its paths include the request wrapper, such as `/graph/cores/egia/layers/common/groups/data/nodes/source/node_type`; IsOS maps them to saved-graph pointers without `/graph` in public diagnostics.

IsOS invokes the **same selected** `egia` launcher with `--capabilities`. It writes one bounded JSON object and exits zero without loading generated config, importing the pipeline, opening a socket, starting EOS discovery, seeding a virtual environment, or writing files. Its target reply is `{"binary":"egia","product_version":"0.1.0","features":["managed-config-v1","eos-gateway-client-v2","egia-evidence-scheduler-v1","vision-evidence-ingress-v1","vision-inspection-uds-v1"]}`. `managed-config-v1` attests that `--managed --no-tui --mode isos` accepts the P04 generated `EGIA_ETC` tree and rejects an internally enabled sandbox. `eos-gateway-client-v2` attests to the existing typed client for `island.eos.v1.GatewayService` and `island.eos.v2.CapabilityService`; it does not attest that EOS implements those services, discovery is complete, or EGIA is ready.  `egia-evidence-scheduler-v1` attests to the managed in-process latest-value scheduler for those exact routes; it does not claim IsOS graph admission. The runtime does not parse the deployment graph, so it advertises **no** graph-version, type-version, or node-type arrays. IsOS checks those arrays on `egia-config`, then checks the runtime's selected launch features independently.

| Field | JSON type / unit | Purpose and validation |
| --- | --- | --- |
| `core` | exact string `egia` / — | Setup identity owner; remains independent of executable basename. |
| `binary` | exact string `egia` / — | Runtime probe identity; must match the selected process role. |
| `product_version` | nonblank string / release | Display and provenance only; no single version is a compatibility predicate. |
| `graph_version`, `node_type_version` | strings / versions | Historical scalar aliases; never broaden an authoritative array. |
| `supported_graph_versions` | nonempty unique string array / exact graph versions | Setup accepts `0.1.0` and `1.0.0` envelopes only. |
| `supported_node_type_versions` | nonempty unique string array / exact suffix versions | Setup accepts `0.1` and historical `1.0`; suffix match alone never admits a type. |
| `supported_node_types` | unique sorted string array / exact full IDs | Setup's four historical common IDs and seven exact EGIA stage IDs; no package type is claimed. |
| `features` | unique ASCII string array / feature tokens | Implemented probe-specific behavior; absent means no claimed feature. |
| `setup_schema_version` | positive integer / schema | Setup request and reply schema remains `1`. |

IsOS probes the exact launcher and resolved interpreter it will launch, with a bounded timeout and stdout. Shell wrappers must not bootstrap a Python environment during capability probing. The selected launcher, interpreter, relevant environment binding, and installed EGIA code are part of one verified deployment input; a probe of one Python installation cannot authorize another. A changed input between probe, P04 generation verification and spawn rejects before stopping an active child. An unreadable interpreter or nonzero, malformed, duplicate-field, oversized or wrong-binary probe fails closed. Probe stderr and local paths are never copied to public diagnostics.

The selected launch chain depends on `EGIA_PYTHON`, `ISLAND_PYTHON`, `ISLAND_PREFIX`, `ISLAND_LIB`, `ISLAND_LIBEXEC`, inherited `PYTHONPATH`, and the source versus installed layout. The wrapper prepends the selected root's module directories to `PYTHONPATH`; the interpreter actually imports `egia.reader_features` from that effective path. IsOS pins the canonical launcher, resolved interpreter, effective nonsecret environment binding and selected module tree for the P04 generation, then rechecks them before replacement. An absent source-checkout `.venv/bin/python` makes `--capabilities` exit `127` without running `uv sync` or creating a virtual environment.

## Derivation and failure boundary

For a `0.1.0` graph with EGIA enabled, IsOS requires setup `setup-v1`, `catalog`, `graph-0.1-reader`, the exact graph and type versions, and each exact full EGIA node ID. For either graph version, a managed EGIA launch requires runtime `managed-config-v1` and `eos-gateway-client-v2`. A safe `1.0.0` graph with only the four historical IDs may use a known legacy setup identity **only when all three version/type arrays are absent** and the scalar aliases are exactly `1.0.0`/`1.0`; a partially present or malformed array fails. This fallback cannot admit an unknown ID or feature, and does not bypass the selected runtime probe for Run or Restart. A valid setup identity on its own never makes the EGIA process launchable.

IsOS checks actual saved graph bytes, registered placement, exact package pins and selected-reader sets before Render, generation selection, active-child stop or spawn. A known registered ID missing from a selected reader's set yields IsOS `reader_type_unsupported`; an unknown/future graph or type retains its earlier G01/G05 exact-path code. A missing advertised feature yields `reader_feature_unsupported`; wrong or malformed probe yields `reader_binary_mismatch` or `reader_identity_invalid`. IsOS returns its P05 `{code,path,message,stage,reader}` diagnostic and supported set; EGIA setup retains schema-1 diagnostics. An upgrade is named only when IsOS verifies one exact compatible release and its asset digest. `0.1.0` support, a process PID and a bound listener never imply R02 Gateway readiness or a complete managed stage launch.

The [frozen fixture](../../../ei/tests/fixtures/reader_features/README.md) covers selected probe identities, all four IDs and placements, legacy fallback, unsupported stage IDs, missing runtime features, malformed probes, changed interpreter and no-stop rejection. A01 adds separate EGIA stage tests; IsOS selected-reader admission remains a separate gate.

**Q01 implemented EGIA probe:** A selected Vision evidence binding additionally requires `vision-evidence-ingress-v1` and `vision-inspection-uds-v1` from the exact managed EGIA runtime `--capabilities` reply. The first attests the bounded authenticated EOS owner-UDS subscriber; the second the separate read-only EGIA inspection UDS and grant. Existing A04 evidence routing does not imply either feature. IsOS rejects a missing token before issuing run grants or replacing children. See [Vision evidence](vision-evidence.md).
