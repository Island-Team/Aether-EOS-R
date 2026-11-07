# IsosUI in IsOS

[中文](README_cn.md) · [IsosUI guide](../docs/isosui/README.md) · [IsOS architecture](../docs/island/architecture.md)

**Unreleased 0.2.1 source target.** IsosUI lives at `ui/` in the unified IsOS repository. IsOS (Island OS) owns one graph core. EOS (Embodied OS) and EI (Embodied Intelligence, formerly EGIA) are separate supervised processes within that graph, and the verified `island/mujoco-sim@0.1.1` package remains external. The historical IsosUI v0.1.0 release and dated test results below remain evidence for their original inputs; no v0.2.1 tag or product release exists.

## Build from the unified checkout

Use Node.js 22 and npm 10. From the repository root:

```bash
cd ui
npm ci --no-audit --no-fund
npm run typecheck
npm test
npm run build
```

`npm run dev` and `npm start` serve the standalone frontend on `127.0.0.1:16001`. The browser uses the IsOS loopback API at `http://127.0.0.1:16000/api/isos/v1` and the EOS bridge at `http://127.0.0.1:16020/api/eos/v2`; an explicit `ISOSUI_API_ORIGIN` or `ISOSUI_EOS_API_ORIGIN` may select another HTTP loopback port at build time. The UI sends no URL token or Authorization header. IsOS serves a verified `dist/local/` bundle for the installed workbench; Island-web hosts a reviewed `dist/hosted/` source-commit build under `/isosui/` and presents it at `/en/isos` and `/cn/isos`.

| Build | Output | Asset base |
| --- | --- | --- |
| `npm run build:local` | `dist/local/` | `/` |
| `npm run build:hosted` | `dist/hosted/` | `/isosui/` |

Each bundle includes `index.html`, `assets/`, `IslandTeam.png` and `manifest.json`. Its manifest declares package version `0.2.1`, the exact source commit, build target and the independently versioned IsOS API schema `0.1.0`. Building a source commit does not create a release.

## Graph and workbench

IsosUI edits graph format `0.2.0` only with a single `isos` core. Manager layers retain their names; EOS domains use `eos_*` layers, EI domains use `ei_*` layers and browser nodes use `ui`. Registered type IDs use `isos.<layer>.<name>@v0.2`. IsOS owns the selected revision-bound catalog and validates every Apply and Run. Saved `0.1.0` and safe legacy `1.0.0` graphs open read-only until IsOS explicitly migrates them; unknown types or future graph versions remain read-only. The editor stores canvas layout locally, separate from graph configuration.

Home shows package receipts, readiness, saved and active revisions, logs and process controls. Graph edits typed settings and ports from exact registered type placement. EOS presents observed hardware and admitted primitive controls from the active graph; EI shows its process state and logs, including historical EGIA process IDs. Browser key input is a `ui` node. A graph edge never sends a command. IsOS owns Stop; EOS Dispatcher owns actuator admission. Vision uses the independent EOS loopback HTTP/WS contract and accepts only run- and revision-matched evidence. For operator steps and exact field rules see [first use](../docs/isosui/usage.md), [catalog](../docs/isosui/catalog/README.md) and [interfaces](../docs/isosui/README.md).

## Historical verification

The dated records below describe the earlier standalone IsosUI v0.1.0 line and its tested partner versions. They are not qualifications of the unified 0.2.1 product.

The standalone frontend was checked on Node.js 22 with `npm test`, `npm run build`, and `npm start`; `/en/isos` returned HTTP 200 at `127.0.0.1:16001`, and the local bundle contained the `127.0.0.1:16000` API address. Complete output is saved locally under `logs/test/20261030-npm-frontend/`.

Licensed under [Apache-2.0](LICENSE).

Graph selection and layout checks: `npm test`, `npm run build`, and `npm start` (`ISOSUI_PORT=16041`); the dev server returned HTTP 200 and warning overlays are disabled while compile error overlays remain enabled. Logs: `logs/test/20261030-graph-selection/`.

The Graph module loads as a separate JavaScript chunk, and webpack emits a small independent runtime chunk. The self-hosted Chinese font files remain optional browser requests; webpack's JavaScript size warning excludes those font assets but still checks JavaScript chunks and the startup entrypoint. The build log under `logs/test/20261030-webpack-size/` records the measured output.

The EOS control workbench was checked on Node.js 22 with `npm test` (25 tests), `npm run build` (typecheck plus both bundles), `git diff --check`, and `ISOSUI_PORT=16041 npm start` followed by `curl -fsSI http://127.0.0.1:16041/en/isos` (HTTP 200). The build emitted no warnings. Full output is under `logs/test/20261030-eos-control/`. Runtime control calls require an applied, active graph and a running EOS bridge; the mock and simulator command tests live with EOS.

The adapter status monitor update was checked with `npm test` (26 tests), `npm run build` (typecheck plus local and hosted bundles), and `git diff --check`. Both bundles compiled without warnings; output is under `logs/test/20261030-eos-monitor-status/`. The EOS bridge supplies `status` with `state` and `active_command_id`; a stopped actuator can remain healthy and continue to expose its last observation. Live requires fresh `ready` status and `healthy` health. Primitive invocation stays disabled until its target is live; the panel asks the operator to restart EOS to rearm a stopped target.

Package node-set UI checks: `npm test` (31 tests), `npm run build` (typecheck plus local and hosted bundles), and `git diff --check` passed. Complete output is under `logs/test/20261030-package-node-sets/`. The editor previews against IsOS without saving; its server-side composition and validation require a compatible IsOS build.

Graph workbench and Vision client checks: `npm test` (41 tests), `npm run build` (typecheck plus local and hosted bundles), and `git diff --check` passed. Complete output is under `logs/test/20261030-graph-vision/`. The binary frame tests cover bounded metadata, dimensions, and JPEG rejection. End-to-end streaming requires a running EOS camera gate and IsOS graph revision.

Vision auto-connect checks: `npm test` (53 tests), `npm run build` (typecheck and both bundles), and `git diff --check` passed. Full output is under `logs/test/20261031-vision-autoconnect/`. Gate selection tests cover canonical and expanded node paths, operator selection, and unavailable links. A live browser stream requires running IsOS and EOS; both loopback APIs were offline during this UI check.

UI selector and build verification (2026-10-31): `npm test` passed 45 tests, `npm run build` passed TypeScript checks and both local/hosted bundles, and `git diff --check` passed. Full output: `logs/test/20261031-build-catalog-visuals/{test,build}.log` (local, ignored). The selector and binary build controls required the matching IsOS endpoints at that time; when unavailable, the UI reported that the option service was unavailable and left existing graph fields readable.

Documentation alignment check (2026-10-31): `npm test` passed 49 tests, `npm run build` passed TypeScript checks and both bundle/manifest checks, and `git diff --check` passed. Full output is in the ignored `logs/test/20261031-docs-code-alignment/` directory. The test output includes a Sass legacy JavaScript API deprecation warning.

Graph action and canvas stability check (2026-10-31): `npm test` passed 52 tests, `npm run build` passed TypeScript and both local/hosted bundle checks, and `git diff --check` passed. Complete output is in ignored `logs/test/20261031-graph-run-ui/`. The UI depends on the IsOS build-status and stale-build recovery API in the matching IsOS update.

Optional core readiness check (2026-10-31): `npm run typecheck`, `npm test` (51 tests), and `npm run build` passed. The Graph page shows an absent core as “Not enabled in the saved graph” in a neutral status line. Full output is in ignored `logs/test/20261031-stale-setup-recovery/`.

Unified Home logs check (2026-10-31): `npm test` (54 tests), `npm run build` (typecheck, local and hosted bundles), and `git diff --check` passed. Output is in ignored `logs/test/20261031-unified-logs-home/`.

Legacy editor removal check (2026-10-31): `npm test` passed 50 tests, `npm run build` passed typecheck and both bundle/manifest checks, and `git diff --check` passed. Full output is in ignored `logs/test/20261031-remove-legacy-editor/`. The current editor and API client remain covered; removed profile-graph tests belonged only to the retired editor.

Additive node-set preview check (2026-10-31): `npm test` passed 51 tests, `npm run build` passed typecheck and both bundle/manifest checks, and `git diff --check` passed. Full output is in ignored `logs/test/20261031-node-set-additive/`. The tests cover rejection of a preview that removes an existing node.

IsOS v1 contract check (2026-10-31): `npm test` passed 53 tests, `npm run build` passed typecheck and both bundles, and `git diff --check` passed. The local and hosted manifests both report IsosUI `0.1.0` and IsOS API schema `1`. Full output is in ignored `logs/test/20261031-version-alignment/`. The IsOS server must use the matching v1 contract before this bundle is deployed.
