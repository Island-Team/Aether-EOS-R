# Graph catalog projection

[中文](../../../docs_cn/isosui/catalog/README_cn.md) · [First use](../usage.md) · [Design](../design.md)

**U01–U04 owner features are merged; cross-owner release remains open.** IsosUI displays the IsOS catalog; it does not register runtime node types or own their payload fields. Read the hierarchy in the same order as first use: choose a **core**, narrow to a **layer**, choose a **group**, then inspect and add a **node type**. The selected type's description explains what it does; the instance description remains editable and explains this placement. The exact type ID, settings, ports, capabilities, defaults, units, validation and compatibility are defined in the owner's `interfaces/` contract, not copied into UI data.

| Level | UI behavior | Owner and example |
| --- | --- | --- |
| Core | Show readiness, package pin and broad status; a disabled core remains visible with reason. | EOS, EGIA and IsOS own their own live state. |
| Layer | Filter major domains without changing the graph. | EOS hardware, process, control; EGIA agent stages; IsOS management. |
| Group | Show related registered types and whether the package contributes them. | EOS camera/base/manipulator, MuJoCo process, EGIA perception/context/decision. |
| Node type | Show nonblank type description, placement, settings and typed ports from registry; offer Add only if compatible. | EOS, IsOS, EGIA and package-owned catalogs define their respective types. |
| Node instance | Show editable label and description, saved settings, errors, runtime health and source edges separately. | IsOS owns saved graph; EOS/EGIA own runtime status. |

The UI looks up an exact `(core, layer, group, type_id)` registry record. It never derives identity from translated labels or folder names. On an old sidecar missing `NodeType.description`, show the server's compatibility description with a fallback marker. If a type is unavailable because a pinned package is missing or invalid, retain the graph's ID and show the reason; block Apply. Unknown future fields remain preserved in the draft until the owner rejects them, and unknown future graph versions open read-only.

The node inspector order is **identity and description → package/version and availability → settings and selectors → ports and wires → validation → live status/log link**. A selector option keeps its stable value; its label and description are presentation. Each wire inspector shows nice priority and frequency cap, including legacy defaults, before the operator applies. The log link opens the current run at its first ready/failure event, then filters by node path and correlation ID. See [workspace algorithms](../design.md) for stale revision, selector refresh and panel persistence.

## Selected catalog compatibility

For a saved `0.1.0` graph, Graph and EOS workbench read the IsOS selected catalog from `GET /node-types/selected` and require its `graph_revision` to equal the graph's saved-byte revision. It includes owner-admitted EGIA stages and their setting and data schema metadata. The global `GET /node-types` catalog remains the source for installed package discovery. If Apply races the two reads, the UI retries the graph and selected catalog once. A second mismatch or unavailable selected owner keeps the raw graph visible read-only and disables Apply and EOS Invoke. Unknown types and future graph versions remain read-only.

EGIA positive integer and positive number settings use numeric editors and retain JSON numbers on Apply. Owner-managed `endpoint` objects are displayed as structured, read-only values with configuration guidance; the editor never converts them to strings. During Run or Restart, Stop remains available on Graph, Home, EOS control, and the workspace bar. A shared coordinator waits for any Run request already in flight, sends a final Stop, and checks the final owner status before reporting a newly started run as stopped. The confirmation identifies that final run ID.

If the first Stop transport fails while a Run is already in flight, the coordinator still waits for that Run and issues one final Stop. A failed final Stop keeps the workspace Stop fence active through a changed run ID until an explicit Stop confirms. An uncertain standalone Stop has no automatic retry.

## U01 implementation evidence (2026-11-02)

The editor reads the registered type description and setting metadata, browses the exact core/layer/group/type tuple, and requests P03 options for each draft. A selector is keyed by exact `(node_path, setting)`; its saved unavailable ID and owner reason remain visible. The MuJoCo device choice updates both adapter device fields in one edit. A superseded POST response is discarded, including after a package change with identical draft bytes. Registered types match the complete placement tuple, and new `@v0.1` nodes receive no guessed hardware values. Save and Save/Run stay disabled until current options arrive and required or unavailable choices are repaired; **Check graph** remains available and IsOS still validates every Apply and Run.

From this repository, run `npm ci`, `npm run typecheck`, `npm test`, `npm run build`, `git diff --check`, and the local-link check in `logs/test/20261102-u01-catalog-selector/`. The full test suite passed (25 files, 126 tests); typecheck, local and hosted builds, manifest verification, and four-document local-link check passed. Complete outputs are under ignored `logs/test/20261102-u01-catalog-selector/`. The final gates ran with Node.js 22.23.4 and npm 10.9.9.
