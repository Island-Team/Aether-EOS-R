# TODO

This is the single implementation roadmap for Island-egia. `[x]` records a verified current foundation, **Partial** records a usable seam with known work, and `[ ]` records work that is not implemented.

## Current verification baseline (2026-09-10)

- [x] Keep the full pytest suite green: 562 tests pass for the contract-aligned MVP.
- [x] Keep Ruff, Pyright, and all import-linter contracts green.
- [x] Keep canonical EOS contracts, checked Python models, and installed schema mirrors in parity.
- [x] Keep `islandctl validate`, prompt preview, and install-layout checks green.
- [x] Run the tracked Q03 source and installed-wheel CI workflow at selected EGIA `2e7abae` ([push job](../island/retired-repository-references.md#ref-075162f7d189), [PR job](../island/retired-repository-references.md#ref-ef872dabc63c)).

## Implemented foundations

- [x] **Contracts and EOS link:** pinned EOS protocol types, checked models, per-backend clients, retrying mandatory discovery, health, primitive discovery, and optional semantic native-skill discovery.
- [x] **Prompt ownership:** one Agent-owned `etc/egia/prompts.toml` for planner, proposer, sentinel, profile writer, and prioritized dynamic context.
- [x] **Perception and context:** bounded observation admission, hearing ingress, body/world sources, grouped hardware, compact object and detection summaries, complete dynamic-message budgeting, and explicit omitted counts.
- [x] **Missions and decision:** prioritized missions, editable semantic plans, primitive-only execution steps, recovery feedback, proposer, sentinel, streamed model I/O, token accounting, and invocation policy.
- [x] **Safety and monitor:** mission-scoped primitive admission, continuous audit, heartbeat and stall monitoring, hard bounds, emergency-stop latching, and direct emergency egress.
- [x] **Memory:** separate working, episodic, and reference stores, keyword and structured recall, documentation retrieval, consolidation, review, and optional sqlite-vec recall.
- [x] **Runtime and operator surfaces:** external EOS profiles, multi-backend launch, terminal and browser interfaces, primitive/skill catalogs, and primitive operator invocation.
- [x] **Coordination:** config-gated EOS device bidders, auction, task board, executor queues, zone leases, rediscovery, and deterministic benchmarks.

## Near-term module work

### Decision and safety

- [x] Make the active plan or execute phase explicit through the concurrent fast controller and slow semantic reasoner; failed execution forces a plan phase.
- [ ] Normalize shutdown outcomes across model-call and step boundaries.
- [ ] Add causal primitive-result provenance for accepted-step invalidation.
- [ ] Implement concurrent-resource and plan-versus-action safety assessment.

### Coordination and fleet

- [ ] Reconcile running zone-lease preemption with mission preemption.
- [ ] Merge scene projections across EOS endpoints while preserving targets.
- [ ] Persist bid and award outcomes as optional recall priors.

### Architecture and quality

- [ ] Remove the `context → memory` back-edge through an injected interface.
- [x] Add CI and an installed-wheel verification job ([merged PR #103](../island/retired-repository-references.md#ref-bd5748ad94f4)).

## EGIA 0.1.0 implementation milestones

Python is the target EGIA runtime. The [node catalog](layers/README.md), [target architecture](design/architecture.md), [single stage interface](interfaces/pipeline.md), and [first-use guide](develop/build-and-run.md) describe the target. Mark a milestone complete only with its acceptance evidence. EOS's Rust gRPC gateway and IsOS's graph/config reader are external dependencies. The 0.x target has no WorkflowService, ActionList or workflow DAG; missions and plans are the agent state model.

| Milestone | Depends on | Output and acceptance gate |
| --- | --- | --- |
| **E0 — contract and compatibility fixtures** | Current Python core and IsOS graph I0 | `egia-config setup` advertises supported graph/type sets while retaining old identity fields. Register `@v0.1` stage descriptions, ports and settings; read known `1.0.0`/`@v1.0` only for established safe behavior. Fixtures cover unknown future versions, optional-field extension and path-specific diagnostics. |
| **E1 — complete managed configuration** | E0, IsOS I1/I3 config layout | Render from complete installed defaults plus validated graph overlays: prompts, model bindings, modes and safety policy. Secrets remain environment references. A revision-selected `EGIA_ETC` snapshot starts from an installed wheel; generated files are never edited in place. |
| **E2 — graph stage composition** | E1, IsOS I1 catalog | Instantiate link, perception, context, missions, decision, safety and monitor inside one Python process. Report canonical node health and correlation IDs. Fixed safety→link and monitor emergency routes reject authored bypass edges. Per-stage mock tests cover empty, malformed, duplicate, overflow and stop behavior. |
| **E3 — EOS gateway integration** | E2, EOS Rust gRPC gateway, MuJoCo 0.1.1 package | Verify discovery, capability and terminal-operation semantics against the public EOS contract. Incomplete discovery blocks ready; reconnect, stale/duplicate operation, cancellation and emergency stop are exercised with a real IsOS-managed simulator. |
| **E4 — shared logging and recovery** | E2–E3, IsOS I3 event envelope | Map sanitized events to run/correlation/node fields without replacing local replay or checkpoints. Test startup once, causal state-change/terminal order, rotation, redaction, lost collector, cursor filtering and fresh discovery after restart. |
| **E5 — first-use integration gate** | E0–E4, IsOS I4–I6 | Installed-wheel and CI gates plus an IsOS → EOS → MuJoCo → EGIA clean-home run cover failure, restart and safe stop. Save full output in `logs/test/<run-id>/`; update the first-use guide with commands and measured results before `0.1.0` publication. |

The [Q03 owner qualification design](design/q03-qualification.md) records E5's selected Q01/Q02 dependencies, installed boundary, fixed benchmarks, metrics, and evidence. The five-run baseline, the passing [EGIA `4db8319c` source-and-installed CI job](../island/retired-repository-references.md#ref-26e039c8e5e4), and the sealed EOS final17 Q01 source-process replay are supplemented by the installed EGIA managed-process receipt in [EGIA PR #108](../island/retired-repository-references.md#ref-cfc2907c41c0) and [EOS PR #347](https://github.com/Island-Team/Island-eos/pull/347). Its input SHA-256 is `3688ba488a91192e5b16b00cba1dae18a8a80e6578e2edad9794616c93b2dbee`; positive and negative seals are `07dad1b546c58f3683798d5fbd225165c8052c2753b6c4f6b15e2d875759367b` and `5e179082da501496e197171bf61a8403b3d0083f0e8295310b08f39583504cd1`. The installed-process E5 owner evidence boundary passed. EOS's Q03 exact-head source reconciliation and Q04 merged-head release qualification remain separate; `0.1.0` is not published by this status.

## Later modules

- [ ] **Governed tools module:** define explicit contracts, policy, isolation, and audit semantics before reintroducing any Agent-local execution surface.
- [ ] **Raster-map processing module:** define a new bounded, model-readable contract before adding storage, processing, or a northbound stream.
- [ ] **Schema-backed native-skill execution:** extend EOS discovery with validated argument schemas before exposing skill invocation; until then skills remain descriptive guidance composed from primitives.
- [ ] **End-to-end model runtime:** measure and optimize perception, context, inference, safety, execution, and feedback.
- [ ] **Trajectory and evaluation:** versioned run capture, replay, dataset governance, and reproducible qualification benchmarks.
- [ ] **Learned perception assistance:** bounded inference behind distribution; `egia.tensor` and package-managed model weights are deferred beyond this release.
- [ ] **Learned world model:** advisory short-horizon prediction and plan scoring.
- [ ] **Learned policy:** supervised and offline-RL proposals under deterministic validation and safety.
- [ ] **Fleet learning:** MARL experiments against deterministic coordination.
- [ ] **Artifact qualification:** versioning, shadow evaluation, promotion, rollback, and audit provenance.

Planned learned modules must not own physical release authority or bypass `decision → safety → link → EOS`. Continuous control and physical safety remain in EOS.
