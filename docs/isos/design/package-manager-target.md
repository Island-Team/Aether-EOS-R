# IsOS package manager: current path and v0.2.1 target

[中文](../../../docs_cn/isos/design/package-manager-target_cn.md) · [Current package admission](packages.md) · [Release contract](../interfaces/packages/release.md) · [Unification plan](../../../ARCHITECTURE-QUESTIONS.md)

**Status:** design for the unified IsOS v0.2.1 implementation. The first section describes current code; the remaining sections specify the target. A target step is not a claim that it is implemented. Package, graph, node profile, process, simulator, and IsOS product versions are independent.

## Current package manager, as implemented

```text
approved Island-port catalog ── exact {id, version} manifest
                                             │
reviewed local manifest ────────────────────┤
                                             ▼
                               select host asset / local bundle
                                             ▼
                        verify SHA, size, paths, payload, adapters
                                             ▼
                    local store + receipt + release + type sidecar
                                             ▼
              graph exact pin → Verify → Validate/Apply → Run/Restart
```

IsOS currently calls the Island-port catalog at `https://www.os-island.com/api/packages/v1` for search and exact release metadata. `isos pkg install` selects a platform asset from a release, downloads over HTTPS, checks its declared size and SHA-256, rejects unsafe archive paths and symlinks, validates declared executables/resources/adapter libraries, and atomically publishes a store directory. `isos pkg link` accepts an explicit local bundle and manifest, records its fingerprint, and requires relinking after changed bytes. The store uses the user's data directory under `packages/<namespace>/<name>/<version>`, with a schema-2 receipt, canonical `release.json`, and optional generated `node-types.json` sidecar. Nothing is installed or started merely because it appears in search results.

Graph nodes select an exact package `{id, version}`. IsOS rechecks the receipt, release hash, complete payload hash, sidecar, registered node type, resource IDs, owner, and lifecycle capability during graph admission and Run/Restart. Removal is refused while saved or active graph state refers to that exact pin. The package ID and executable filename do not select special runtime behavior. EOS owns its hardware adapters and command admission; an IsOS-verified package declaration does not grant an actuator route. The exact published MuJoCo 0.1.1 schema-4 manifest has a digest-locked import adapter in current code; arbitrary schema-4 packages are rejected.

Current CLI includes search, info, install, link, list, verify, remove, node-set preview, publish, and upload. Current HTTP includes list, install, link, and preview-link under `/api/isos/v1`. The published package contract is `isos.package@v1` with manifest schema `0.1.0`. There is **no general dependency solver, transactional package upgrade, offline registry cache, signed registry trust root, or GitHub-based IsOS self-update workflow** yet. Current `version check` and HTTP version route report local identity/readiness; they are not a latest-release checker.

## Target ownership map

| Owner | May do | Must not do |
| --- | --- | --- |
| IsOS package manager | Verify registry metadata, manifests, assets, local receipts, compatibility, graph references, and staged updates; maintain installed inventory and audit evidence. | Interpret an opaque hardware payload as authority, start an unadmitted process, silently change an applied graph. |
| IsOS graph manager | Pin exact package releases and node/profile versions; atomically validate and apply revisions. | Resolve `latest` at Run, auto-rewrite user graph settings, or bypass package verification. |
| EOS | Validate EOS-owned package contributions, supervise its simulator/backend child when required, and control all actuator admission and Stop through Dispatcher. | Trust package metadata alone as a device grant or create a second actuator route. |
| EI | Validate EI-owned extensions and run intelligence workloads under its lifecycle contract. | Directly write the applied graph or actuators. |
| Public registry | Publish reviewed, non-sensitive release metadata and immutable assets for public packages. | Hold real hardware setup, secrets, calibration, device identities, or private debug evidence. |
| Private registry / `isos-p` | Hold access-controlled hardware packages and setup evidence, with the same package contract and verification rules. | Leak private metadata into the public index, source snapshot, logs, or release notes. |
| IsosUI | Present search, installed inventory, plans, verification and update consent through IsOS APIs. | Download, install, or start packages independently of IsOS. |

`isos-p` is the private **source** repository. A private package registry is a separately controlled distribution channel; the source repository itself is not automatically a safe package registry. Registry credentials live in the user's credential store, never in graph files or tracked configuration.

## Version and support map

A checked-in `config/version-support.toml` is the local compatibility authority for the IsOS build. It is generated or verified against the actual registered contracts in CI. It names the product version, accepted graph formats, exact package manifest identities, node/profile versions, IsOS↔owner process contracts, EOS/EI interface contracts, and package constraints. The runtime uses this map whether online or offline. Registry metadata can offer an update but cannot widen compatibility for an already installed binary.

**Current implementation slice:** the bundled map reports product, target graph, HTTP, package, and pinned MuJoCo identities through the CLI and HTTP version surfaces. Package manifest admission now reads the map for its contract and schema, and the exact published MuJoCo asset pin comes from the map. IsOS setup identity now requires EOS/EI product version 0.2.1 before launch and readiness. Apply/Run graph and other owner checks still use their existing validators; wider admission integration and CI consistency checks remain target work.

| Identity | Example/current value | Target check |
| --- | --- | --- |
| IsOS product | `0.2.1` | Display, provenance, and release selection; never a substitute for interface checks. |
| Graph format | `0.2.0` | Exact accepted format and `isos` core; migration is explicit. |
| Registered type/profile | `isos.<layer>.<name>@v0.2` | Exact type ID, placement, settings, ports, capability and owner. |
| Package manifest | `isos.package@v1`, schema `0.1.0` today | Exact contract/schema accepted by the importer; live store contains normalized current records. |
| Package release | `{id, version, source commit, asset digest}` | Exact immutable pin and host-matched asset. |
| EOS/EI/process wire | Independently versioned contracts | Handshake on exact supported contract set before ready; reject mismatches and stale revision/generation. |
| MuJoCo | `island/mujoco-sim@0.1.1` | Exact approved manifest and asset digest plus simulator protocol and scene declarations. |

The support-map record for an external package contains its accepted manifest identity, owner, required process/adapter contract, platform constraints, minimum/maximum supported package version only if the owner has tested that range, and optional exact digest allowlist. Prefer exact versions and digests for active graphs. A package's SemVer does not imply wire compatibility. `isos version check` returns `compatible`, `migration_required`, or `unsupported`, naming each failed field and supported value. It never silently imports old data or changes an active run.

## Registry model

For v0.2.1, keep **Island-port as the public package metadata catalog**, use **GitHub for the public IsOS product release and approved package assets where appropriate**, and use a separate authenticated channel for private hardware packages. The registry client interface remains independent of the host. Use two package channels under that client contract:

1. **Public:** a reviewed, immutable release index for non-sensitive packages, with search/discovery, exact release metadata, asset digest/size/platform, contract declarations, publisher provenance, review status, and revocation status. Anonymous reads can work if available; author publication requires authentication and review.
2. **Private:** an access-controlled index for real hardware packages, driver builds, calibration-dependent adapters, and field evidence. Login grants discovery and download; a package's presence is not authority to control a device. The public search response must not reveal private package names or metadata.

A registry entry is metadata, not executable trust. The target trust chain is a pinned registry trust root plus signed, expiring index/release metadata and content-addressed assets. CI and clients verify signatures, freshness, rollback protection, asset digests, and publisher/review policy. Until this trust chain is implemented, the existing exact approved catalog and digest verification remain the documented current boundary; do not claim signatures are present. An offline cached index may be used only within its signed validity period. A stale cache can display inventory but cannot authorize a new install. Installed exact pins remain usable offline after full local verification if their policy has not been revoked in the last trusted snapshot; define revocation expiry explicitly before implementation.

**Publication workflow:** author builds a package and manifest → local `pkg validate` checks schemas, bounded paths, exact assets, node contributions, owner contracts, and platform claims → run owner tests → submit metadata and asset to public or private review channel → reviewer checks disclosure, license, provenance, test evidence, and digest → registry signs/publishes immutable release → clients can discover it. Corrections create a new release; do not overwrite bytes under an existing `{id,version}`. Revocation marks a bad release unavailable for new installs and gives a bounded reason; it does not silently delete an installed package or stop a running actuator without an explicit safety policy.

## Operator workflow map

```text
Discover / local source
        │
        ▼
Inspect exact release + channel + contracts + platform + provenance
        │
        ▼
Plan: compare local support map, graph pins, owner capability, dependencies
        │
        ├── incompatible → diagnostic with exact field; no mutation
        ▼
Consent to install/link/update
        │
        ▼
Fetch to staging → verify registry policy + manifest + digest + safe tree
        │
        ├── failure → delete staging; installed/graph/run unchanged
        ▼
Atomically publish immutable package version + receipt
        │
        ▼
Validate proposed graph revision and owner projections
        │
        ├── failure → keep package installed but unselected; run unchanged
        ▼
Operator Apply exact revision → readiness preflight → Run/Restart
        │
        ▼
Monitor; Stop safely; remove old unreferenced package explicitly
```

| Operation | Required inputs | State effect | Rejection behavior |
| --- | --- | --- | --- |
| Search/info | Channel, query or exact ID/version | Read only. Online result is discovery, not approval. | Offline/auth failure reports status; local inventory still works. |
| Plan install | Exact release, platform, local support map | Read-only compatibility and dependency plan. | Precise unsupported contract, missing dependency, platform, or policy path. |
| Install | Exact release and operator consent | Publish immutable verified store version; graph/run unchanged. | Remove staging, keep prior version. |
| Link (development/private) | Exact local manifest and bundle | Record verified local fingerprint; graph/run unchanged. | Changed bytes require explicit relink; no automatic trust of a local path. |
| Verify | Exact installed pin or all selected pins | Read only; hash receipt, manifest, payload, sidecar, adapter files. | Fail closed with bounded diagnostic; no attempted repair in Run. |
| Select / Apply | Exact graph pin and revision | Atomically changes desired graph only after all owners validate. | Saved bytes/revision and active run unchanged. |
| Run / Restart | Applied revision, verified exact pins and owner readiness | Starts a revision-bound generation; EOS controls actuator safety. | No partial healthy result; safe Stop/uncertain outcome rules apply. |
| Plan package update | Existing pin, proposed exact new pin, dependency and graph migration plan | Read only; show operator diff and rollback preconditions. | Incompatible package cannot be staged as active replacement. |
| Commit package update | Explicit consent, verified staged release, accepted graph migration | Atomic graph revision change, then controlled restart; old package remains for rollback. | Failure retains old pin/run when possible; uncertain Stop prevents restart. |
| Remove | Exact installed pin | Delete only if no saved/active/recovery reference. | Refuse referenced package and name each reference. |

No `latest` value appears in an applied graph. Dependencies, if supported, must form a bounded acyclic graph of exact versions; a solver may *propose* pins but never silently choose them. A v0.2.1 implementation may reject undeclared transitive dependencies rather than ship a partial solver. Updates are staged side by side, so rollback has the old verified package and old graph revision available. A rollback is never automatic across an uncertain actuator Stop.

## IsOS product update versus package update

`isos update check` compares the installed IsOS product version with the latest **published** public `Island-Team/isos` release when network is available. IsosUI offers the same explicit check action; neither performs an automatic startup check. If GitHub access requires login, prompt for GitHub login; startup, Run, and local compatibility checks do not require login. If offline or unauthenticated, report `online status unknown` and evaluate the local support map. Never imply that a GitHub release alone makes local graph, packages, or wire contracts compatible.

`isos update/upgrade` presents the exact target release, notes, supported-version map, graph/package migration effects, asset digest, and rollback limits. Ask before download or install. Verify the release provenance and asset bytes, stage a new IsOS installation, run compatibility preflight, stop children safely, switch atomically, and verify readiness. Keep the old installation and data snapshot until the new generation is healthy. No automatic self-update or external package update occurs on server startup. The automated switch is a later implementation slice unless all Stop, rollback, and signature tests pass for v0.2.1.

## Hardware information and public disclosure

All **real hardware-specific** information belongs in `isos-p` or a more restricted private store: robot IDs/serials, calibration, device profiles, driver endpoints and LAN topology, credentials, field logs, captures, and debugging traces that identify a physical setup. Public `isos` may contain generic hardware interfaces, synthetic fixtures, mock adapters, redacted examples, and reproducible tests that use no real device identity. The public snapshot process must scan tracked source, every new commit, archives, generated bindings, docs, and fixtures; review each finding by owner. Do not use absolute user paths or true deployment configuration in either repository's reusable examples. Supply `.example` files and documented defaults, and keep active setup files ignored.

## Implementation slices and acceptance

1. **Current-path inventory:** document actual catalog endpoints, manifest/receipt schemas, all pin locations, launch ownership, and remaining legacy imports. Keep existing exact-pin verification passing.
2. **Support map:** add the versioned file/schema, parser, consistency check against registered contracts, local CLI/HTTP status, mismatch diagnostics, and offline tests.
3. **Registry boundary:** separate public/private channels, authenticated private discovery, signed metadata and cache policy, immutable releases, revocation, and disclosure review. Do not couple registry login to IsOS startup.
4. **Package plan/update:** exact dependency and compatibility plan, side-by-side staging, operator consent, graph revision update, safe restart, rollback/uncertain outcome tests.
5. **Product update:** optional GitHub check and login, release verification, consent, staging, safe switch, rollback, offline fallback.
6. **Qualification:** clean-home install/link → graph Validate/Apply → owner readiness → camera/admitted control → failure/restart → Stop; test changed payload, stale index, wrong signature/digest/platform/contract, expired cache, revoked release, failed owner, and uncertain Stop. Save complete logs under `logs/test/<run-id>/`.

**Remaining implementation choices:** define the signed registry trust root and revocation freshness window, and choose the exact IsOS update asset/rollback packaging. A standalone one-time importer handles supported old graph/package data; IsosUI checks online only on explicit action. These choices do not change the current exact-pin install behavior without a reviewed contract migration.
