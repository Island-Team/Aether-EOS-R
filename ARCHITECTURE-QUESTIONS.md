# IsOS repository unification plan

Working plan, updated 2026-11-06. This file is the decision and conflict record for merging four repositories into one IsOS source tree. **v0.2.1 is the final product version for this implementation.** Publish it from the public `isos` repository only after the full implementation and release gates pass; do not tag or release it during the merge.

## Outcome and scope

- **Private development repository:** rename `Island-Team/Island-eos` to `Island-Team/isos-p`. It holds complete development history and any material that is inappropriate for the public source snapshot. Keep it private.
- **Public source repository:** rename the current public `Island-Team/Aether-EOS-R` to `Island-Team/isos` after moving the old public `Island-Team/isos` to a temporary legacy name. The new public tree is a disclosure-reviewed snapshot of the private tree, with a reproducible source-to-snapshot receipt.
- **One product:** IsOS (Island OS), final release version **v0.2.1** for this implementation. Publish that release from the public `isos` repository after the merge, implementation, review, public snapshot, site pin, and historical asset preservation pass. The existing public EOS 0.2.0 release stays unchanged.
- **One repository:** IsOS, EOS, EI, and IsosUI source live in the unified repository. The MuJoCo simulator stays an external verified package, `island/mujoco-sim@0.1.1`.
- **Hardware privacy:** all real hardware identities, calibration, device profiles, endpoints, captures, and field-debug evidence remain in `isos-p` or a more restricted private store. Public `isos` contains only generic contracts, synthetic fixtures, mock adapters, and redacted examples.
- **Registry split for v0.2.1:** Island-port remains the public package metadata catalog. GitHub hosts the public IsOS product release and approved package assets where appropriate. A separate authenticated private channel serves real hardware packages. IsOS uses one registry-client interface so hosting can change without changing package identity or graph pins.
- **This merge milestone:** preserve histories and current work, reconcile source layout/builds/docs/versions, prove each existing component still builds and runs, prepare public source and release notes, and complete repository cutover. Define future core boundaries and versions in docs. Do **not** make a full redesign of every core link, wire interface, or node profile a precondition for this repository merge.
- **Future architecture milestone:** IsOS server is the top-level launcher and becomes the graph/configuration/lifecycle authority; IsosUI is its client; EOS and EI can run independently or as supervised children. Align their interfaces, profiles, and lifecycle protocol in focused follow-up work. Current runtime safety and Dispatcher ownership must remain intact during the merge.

## Naming

**Use EI — Embodied Intelligence** as the target name. It is clear beside EOS — Embodied OS, short enough for code and graph IDs, and already understood in the project. I do not see a better name that justifies another rename now. New product-facing text, directories, package names, and future contracts use EI. Existing EGIA history is preserved in Git and legacy release evidence. The long-term target is no live `egia` alias or old-name protocol; removal timing is recorded below because it can break current integration and should be done as a focused migration after the repository merge.

## Source layout and module ownership

| Area | Target location | Owner / merge rule |
| --- | --- | --- |
| IsOS server, graph, package installer, manager | `crates/isos/` | IsOS launches and supervises the system; it owns desired graph/configuration, package verification, and orchestration. Keep one source of truth for applied revision. |
| EOS Rust runtime and thin public launchers | `crates/aether-*/` plus EOS launchers | EOS owns device adapters, Dispatcher admission/journal, health, and safe Stop. Preserve its existing qualified behavior. |
| Optional EOS C++ sidechain | `adapters/eos-cxx/` | This still contains legacy runtime code as well as native adapters. Preserve current callers now; later remove unused runtime modules after last-caller review. |
| EI Python runtime and contracts | `ei/` | EI owns intelligence and its local safety checks; no direct actuator route. |
| IsosUI frontend | `ui/` | UI is a client of IsOS; retain its own build and hosted mode. |
| Shared contracts, fixtures, docs, scripts | One owner-designated path each | Deduplicate by owner and content. Do not silently choose between conflicting definitions. Record the source path and imported commit. |
| MuJoCo package | External `island/mujoco-sim@0.1.1` | Verify exact published package, assets, digest, and runtime protocol; do not vendor its source into IsOS. |

## Version map

The unified product version and internal contract versions are independent. A source version bump does not silently change a wire contract or node profile.

| Surface | Merge target | Later architecture target / rule |
| --- | --- | --- |
| Public IsOS product release | **v0.2.1**, after all gates | Release notes describe the four-component merge, EOS 0.2.0 carried-forward behavior, limits, and exact verification. |
| IsOS/EOS Cargo, EI Python/CMake, IsosUI npm | `0.2.1` | Keep source package versions aligned for the unified release. |
| Unified graph | Target `0.2.0`, type IDs `isos.<layer>.<name>@v0.2` | IsOS validates and publishes one applied revision. Legacy migration policy and current runtime acceptance are separate. |
| IsOS HTTP API | Current implementation still exposes `/api/isos/v1` with `schema_version: 0.1.0` in places | Design a separately versioned current API in a follow-up contract milestone; clients negotiate or match exactly. Do not label the old API as new merely because source is 0.2.1. |
| IsOS↔EOS/EI lifecycle | Current owner projections and process launch paths | Define a versioned local management contract with revision/generation fencing, readiness, health, Stop, and restart. Prefer process IPC; reserve FFI for narrow in-process adapters. |
| EOS admission and primitive contracts | Preserve existing independently versioned EOS v2 managed admission where used | Revise only with an explicit contract version and coordinated clients. Dispatcher remains sole actuator admission route. |
| Node profiles, settings, capabilities, ports | Preserve current qualified definitions during merge | Give each registered type/profile its own version; reject unknown IDs or incompatible fields. A profile version is not the product version. |
| Package manifest / simulator protocol | Preserve exact verified MuJoCo 0.1.1 artifact and compatibility import | Package format and simulator protocol have independent IDs/versions and exact digest checks. Do not infer compatibility from source release number. |

**Strict version target:** every current boundary advertises and checks its own contract ID and version; unknown major/minor versions, missing versions, unregistered graph types, stale graph revisions, and mismatched package digests fail closed. During this merge, do not claim this policy is already implemented everywhere. Record each existing gap as follow-up work. An offline import/conversion tool can read historical data without making the running core accept old communication contracts.

### Version check and update design

Maintain a reviewed, tracked **version support map** as the local source of compatibility truth. Its entries specify the IsOS product version, supported graph format, IsOS API and process-management contracts, EOS/EI interface ranges, node-profile versions, package manifest versions, and exact external package constraints. The map is bundled with each IsOS build, tested against the actual registries and manifests, and reported by `isos version` and the server version endpoint. Runtime admission uses this local map even when online; a GitHub release page never overrides compatibility rules in the installed build.

**Implementation status:** the first `config/version-support.toml` slice is bundled and reported by the CLI/HTTP version surfaces, with exact product/schema checks and local graph status. Apply, Run, and package admission still use their existing contract validators; wiring the map into those admission paths is a follow-up before claiming it is the sole compatibility authority.

Proposed tracked path: `config/version-support.toml`. Its schema should have one exact product version and explicit lists, not permissive version ranges by default:

```toml
schema = "isos.version-support@v1"
product = "0.2.1"
graph_formats = ["0.2.0"]
node_type_family = "isos.*@v0.2"

[[external_packages]]
id = "island/mujoco-sim"
version = "0.1.1"
# The exact release manifest and asset digests are recorded in the checked-in
# package catalog, not in a user-specific setup file.
```

Add named sections for actual IsOS HTTP, process-management, EI, EOS, profile, and package contracts only when those contracts are defined and verified. Build/test must reject a map that names a nonexistent contract or fails to list a registered current one. A local version check returns `compatible`, `migration_required`, or `unsupported` with the exact mismatched field and supported values; it never silently upgrades a graph or node profile.

`isos update check` and an explicit IsosUI action check the public `Island-Team/isos` GitHub release when network is available. There is no automatic startup check. If GitHub access requires authentication, ask the operator to log in with GitHub; do not require a login merely to start IsOS. The online result shows the latest published version, release notes, verified asset digest, and whether the local support map says an update path exists. If no network or login is available, report `online status unknown` and run the local version/support check. Never treat a failed online check as a compatible version.

`isos update` or `isos upgrade` is an explicit operator action. Show the proposed version, migration steps, compatibility changes, and rollback path; ask before downloading/installing. Verify signed or checksum-pinned release assets against the public release record and local policy, stage atomically, then revalidate graph and package dependencies before restart. Do not silently update child processes or external packages. The first merge milestone needs the map schema, local checker, and documented online/update workflow; automated upgrade may be a focused follow-up if it is not safely implementable for v0.2.1.

The support map is not a machine-specific configuration file. Examples use generic values, documented defaults, environment overrides where appropriate, and relative or XDG-standard paths. Never commit true credentials, tokens, absolute home paths, local device IDs, user-specific secrets, or deployment-specific active configuration. Keep local config ignored and provide `.example` templates.

The complete [package manager current-state and target workflow](docs/isos/design/package-manager-target.md) specifies registry channels, trust, exact-pin installation, staged updates, rollback, GitHub release checks, and hardware disclosure boundaries. It distinguishes implemented behavior from the target architecture.

## Merge sequence

1. **Preserve:** capture every source repository head, branch, tag, release asset, checksum, and original commit ID. Include unpublished IsOS commits, current dirty EOS/EI/UI work on reviewed topic branches, and ignored evidence. Do not stage unrelated local edits to `CONTRIBUTORS.md` or the deletion of `contributor.md`.
2. **Import:** bring IsOS, IsosUI, and EGIA histories under temporary prefixes into private Island-eos. Keep source-to-import mapping. Reconcile root files and duplicate fixtures only after ownership review. Use focused `refactor(v2)/<topic>` branches and PRs under repository conventions.
3. **Compose:** one Cargo workspace, one CMake composition root, one EI Python project, and one IsosUI frontend directory. Replace sibling-repository paths with in-repository paths. Regenerate bindings from the in-repository source contract. Keep independently runnable EI and EOS entry points.
4. **Version and document:** set all source package versions to 0.2.1. Update English and Chinese current docs, relative links, launch/build instructions, catalogs, and release draft. Mark future architecture targets as targets, not completed behavior. Preserve historical qualification claims with dates.
5. **Verify:** Rust fmt, Clippy, tests; EI Ruff/Pyright/import rules/tests; UI typecheck/tests/local and hosted builds; CMake/CTest; graph/package fixtures; bilingual links; clean-home MuJoCo package link and real Run/Restart/Stop. Keep complete ignored logs in `logs/test/<run-id>/`.
6. **Review conflicts:** for any conflicting module or contract, add an entry below with both sources, current callers, recommended target, migration impact, and decision. Ask before choosing. Defer interface redesign that is not required for source composition or safety.
7. **Public snapshot:** freeze a reviewed private commit. Build a single sanitized public snapshot commit from the existing public EOS base; do not push the current unpublished candidate history because it contains home paths in an intermediate commit. Scan every new public commit and archive for sensitive data, compare snapshot to private with explicit exclusions/overlays, then build/test it. Preserve EOS 0.2.0 history.
8. **Dependent site and legacy releases:** pin Island-web to the reviewed public source commit and verify asset digest plus `/en/isos`, `/cn/isos`, `/isosui/`. Keep the old hosted UI archive. Mirror old IsOS and IsosUI v0.1.0 assets/checksums as labeled legacy preservation releases.
9. **Cutover and release:** after all checks pass, give you exact GitHub web operations to rename old public IsOS temporarily, rename private Island-eos to `isos-p`, rename public Aether-EOS-R to `isos`, verify URLs/releases/assets/site, then delete retired old IsOS/IsosUI/EGIA GitHub repositories and local folders. Keep the external MuJoCo repository and package untouched. Publish public **v0.2.1** only after the unified implementation, verification, and release notes are complete; do not create its tag during earlier stages.

## Conflict register

| ID | Conflict / evidence | Suggested target | Decision / state |
| --- | --- | --- | --- |
| C1 | Prepared graph Apply patch `27890bc` overlapped a private Clippy-only edit in `crates/isos/src/domain/graph/graph_migration.rs`. | Keep the correct Apply implementation and needed lint behavior. | **Resolved in `5aded4ea`:** IsOS tests, formatting, strict Clippy, and real-owner fresh v0.2 HTTP Apply passed. No architectural conflict. |
| C2 | Earlier plan requested live legacy graph inspection and EGIA aliases; your new direction requests current-only runtime interfaces and no old name. | Use a standalone, one-time importer for supported old graph/package data. The running server and owner communications accept only current contracts. Remove live aliases after callers are switched. | **Resolved design:** offline importer is the compatibility boundary; old live modules are not the target. |
| C3 | Published MuJoCo 0.1.1 uses numeric schema-4 manifest; IsOS's current package verifier expects `isos.package@v1`. | Verify the exact published asset and normalize at import, then persist a current internal package record. Keep live runtime contract strict. | Prior answer: “follow what IsOS needs.” Continue with exact-asset import; report any new conflict. |
| C4 | Current IsOS HTTP routes use v1/0.1.0 while graph target is 0.2.0. | Preserve honest version labels during merge; specify new API contract and migration in later architecture milestone. | Future design decision; do not silently relabel. |
| C5 | Existing EOS, EI, IsOS, and IsosUI definitions may overlap in graph types, configuration, fixtures, or ownership. | Make IsOS the graph/config owner; EOS the actuator owner; EI intelligence/safety; UI client. Record concrete overlapping files/type IDs before choosing either implementation. | Audit each concrete collision and ask you first. |
| C6 | Unpublished public candidate branch has unredacted home paths in an intermediate commit. | Rebuild as one sanitized snapshot commit from final private head; preserve already-public EOS 0.2.0 history. | Approved redaction; implementation pending final private freeze. |
| C7 | Current package docs and fixtures contain some real-machine paths/evidence alongside generic hardware interfaces. | Keep real hardware debug and setup only in private `isos-p`; public snapshot gets generic/synthetic/redacted equivalents. | User decision recorded; audit and public snapshot filtering pending. |
| C8 | EI's installed product template and internal launcher still use `[components.egia]` and `bin/egia`, while the unified target name is EI. | Keep the current launcher behavior through this shallow layout move; replace the template, parser, launch path, tests, and external callers together in a focused EI contract migration. | Documented target; do not rename only the template and break installed launch. |

## Settled operating choices

- A standalone one-time importer may read supported old graphs and the exact pinned MuJoCo 0.1.1 manifest. Live IsOS/EOS/EI interfaces accept only current contracts in the target architecture.
- Online update checks happen only when the operator runs `isos update check` or selects the IsosUI check action. Startup and Run use the local support map and never wait for GitHub login or network.
- All true hardware debug/setup details belong in `isos-p`. The public snapshot receives a file-by-file disclosure review for other sensitive information.

The other design choices can be made in later focused interface milestones without delaying repository consolidation. I will add a concrete conflict here when an implementation collision is found, with a recommendation and its effect, before resolving it.
