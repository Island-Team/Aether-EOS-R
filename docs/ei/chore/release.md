# Release Checklist

This document is a **pre-release checklist**: what to prepare and verify before cutting a release. It is not a release procedure, versioning policy, or changelog — those live elsewhere.

The [public release procedure](public-release.md) describes the independent `Aether-EGIA-R` source snapshot. The EOS 2 capability-operation adapter is implemented on the development branch; publication remains blocked until real-process integration passes against an exact published EOS release.

Run every item; a release is blocked until all boxes are checked.

## Cleanliness

- [ ] Development-only `scripts/ssh-tunnel.sh` and `scripts/ssh-tunnel.py` removed before release.
- [ ] Apache-2.0 source review complete; retain required third-party notices and exclude private extensions and deployment data from the public snapshot.
- [ ] Runtime reports under `var/` reviewed — confirm no session output or local state is tracked or included in the wheel.
- [ ] `.gitignore` reviewed — confirm it still enumerates all excluded artifacts (`var/`, captures, `.env`, credentials, machine-specific config) and that nothing on that list was force-added (`git status --ignored`).

## Package integrity

- [ ] Wheel contains generated protobuf bindings and only EGIA-owned JSON Schemas below `egia/contracts/schemas/northbound/`; no EOS-owned JSON Schema mirror is present.
- [ ] Unit tests excluded from the wheel (`*.tests`, `*.tests.*` — unzip and check).
- [ ] `libexec/egia_ext/` absent from the wheel — out-of-process backends are not distributed with the package.

## Gates

- [ ] Full test suite passes (`uv run pytest`).
- [ ] `uv run ruff check .`, `uv run pyright`, and `PYTHONPATH=src:libexec uv run lint-imports` pass.
- [ ] `PYTHONPATH=src:libexec uv run python -m island_admin.schema --check` passes: every JSON Schema in configured logical `eos/contracts/` and `eos/schemas/` loads with a unique `$id`, and `eos/contracts/eos/gateway.proto` exists.
- [ ] Canonical schemas match `docs/island/eos-contracts.md`; any breaking schema change carries a version bump.
- [ ] EGIA uses the selected EOS 2 `CapabilityService` without bypassing EGIA safety admission, and generated protobuf, action versions, and schema paths match the exact EOS release commit.
- [ ] Real EOS integration covers operation submission, duplicate IDs, terminal acknowledgement, timeout, cancellation, skill discovery, and resource streams.
- [ ] `./bin/islandctl validate` passes from the checkout.
- [ ] `scripts/print_prompt.py` and `scripts/print_prompt.py --json` run from an unrelated working directory without EOS or a model and cover every lane.
- [ ] Prompt inspection contains no synthetic time, map section, retrieval handle, or removed execution vocabulary, and its hardware/primitives match production rendering.
- [ ] `etc/egia/missions.toml` loads strictly, every configured dependency graph is acyclic, staged install includes it, and the global planner prompt contains no competition-only instructions.
- [ ] Public Mission templates contain no credentials, private endpoints, machine-specific paths, personal data, or unlicensed third-party material.
- [ ] EGIA-only CMake configure, `egia-dist`, and a staged install complete; every root from `ISLAND_PRODUCT_CONFIG` is merged into the installed `share/island/` tree without collisions, and generated `island.toml` selects `share/island`.
- [ ] Staged install excludes `libexec/egia_ext/` with the default `EGIA_INSTALL_PRIVATE_EXTENSIONS=OFF` while retaining `island_admin`.
- [ ] The public snapshot's `bin/egia` starts the core runtime on a TTY when the private operator console is absent; IsOS starts it with `--no-tui`.
- [ ] IsOS launches EGIA with `--managed --no-tui`; a config with `[launch] sandbox = true` fails before spawning an internal sandbox.

## Configuration & state

- [ ] No compiled-in fallback endpoint; startup with a missing config root fails with the resolved path in the message.
- [ ] `./bin/island` forwards to `isos start`; legacy `--config` and `--no-tui` fail with migration help. Standalone `./bin/egia --island-config <path>` and `islandctl --config <path> validate` use the same configured shared roots.
- [ ] Committed mode presets in `etc/egia/modes.toml` cover loopback only; no deployment gateway endpoint present.
- [x] Browser origin is restricted to the public WebUI at `https://www.os-island.com`; non-browser clients may omit `Origin`.
