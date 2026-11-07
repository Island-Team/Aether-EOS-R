# IsosUI 0.1.0 release and Island-web promotion

[中文](../../../docs_cn/isosui/interfaces/releases_cn.md)

**Owner:** IsosUI owns the source tag, local and hosted bundle archives, their SHA-256 checksums and the release. Island-web owns its pin, website build, PR and deployment. The IsosUI release is not proof of a deployed public HTTPS page.

Run the manual **Release IsosUI** workflow only after the reviewed source is merged to IsosUI `master`, its CI passes, and an exact `v<package.json version>` tag points to that current master commit. Select `promotion=manual` when the cross-repository dispatch/mirror tokens are unavailable; select `promotion=dispatch` only when `ISLAND_WEB_DISPATCH_TOKEN` is configured in IsosUI and Island-web has `ISOSUI_MIRROR_TOKEN`. The workflow rejects the wrong version, a tag off current master, an existing release, and a missing dispatch token before publication. It runs Node 22 tests, release negative tests, typecheck, both builds and bundle verification. Immediately before `gh release create`, it checks remote `master`, the remote tag's peeled commit, and existing release again, since any may change during the build. It then publishes exactly the local and hosted archives plus `SHA256SUMS` after checking both archive digests and their embedded `manifest.json` identities.

For manual Island-web promotion:

1. Download the three assets from the IsosUI release. Check `sha256sum -c SHA256SUMS`, and record the hosted archive digest, release tag and exact tagged commit.
2. On an Island-web `refactor(v2)/<topic>` branch from current `master`, update `scripts/isosui-release.json` to `{ "source_commit": "<tagged 40-character SHA>", "release_tag": "<tag>", "api_schema_version": 1, "sha256": "<hosted archive SHA-256>" }`. The pinned API schema must equal the hosted archive's manifest. Its installer checks the archive digest and extracts only safe paths. Do not substitute a commit-built bundle for a release archive.
3. Run Island-web `npm ci --no-audit --no-fund`, `npm run build`, and verify `dist/en/isos/index.html`, `dist/cn/isos/index.html`, `dist/isosui/manifest.json`, plus the hosted manifest's version, source commit and `build_target: hosted`. Save output in `logs/test/<run-id>/`, open a PR to Island-web `master`, obtain review, then merge.
4. After the website deployment succeeds, check the public `/en/isos` and `/cn/isos` pages and their `/isosui/` assets against the pinned digest/manifest. Exercise the first-use path against an available loopback IsOS/EOS owner, including Validate/Apply, readiness, a correlated primitive result, current-run logs and Stop. Browser local-network permission may be needed for HTTPS to loopback.

The dispatch route asks Island-web's mirror workflow to create that pin PR; it does not itself deploy the page. If dispatch fails after release, the release stays published and the same verified assets may be promoted manually. Do not rerun release creation for an existing tag.

## Q04 preflight evidence

The focused topic was checked with Node 22.23.4 and npm 10 on Linux. Reproduce from its exact committed HEAD with `npm ci --no-audit --no-fund`, `npm test`, `node --test tests/release-preflight.node.mjs tests/release-boundary.node.mjs`, `npm run typecheck`, `npm run build`, and `git diff --check`. The release tests create local and hosted fixture archives, reject altered bytes, a wrong embedded source commit, a symlink member, missing or repeated checksum entries, a wrong version or mode, and a missing dispatch token. The final publication boundary fixture uses a local Git remote to prove unchanged lightweight and annotated tags allow creation while a moved master, retargeted remote tag, or existing release prevents it. Build verification requires the exact HEAD in each manifest. Complete local output is under ignored `logs/test/20261104-q04-release-preflight/` and `logs/test/20261104-q04-release-boundary/`. This preflight topic itself created no tag, release or Island-web deployment; the later publication is recorded below.

## Published v0.1.0 evidence

The annotated [`v0.1.0` tag](../../island/retired-repository-references.md#ref-eb78d2736c67) peels to IsosUI `master` commit `ff4f8800f44485325d767e56fbf1ea10c2046d68`. [Release workflow run 41709462695](../../island/retired-repository-references.md#ref-68283862edb9) succeeded from that commit and published the two archives and `SHA256SUMS`. The checksums downloaded from that release verify as follows; preflight archives built locally have different bytes and are not these assets:

| Release asset | SHA-256 |
| --- | --- |
| `isosui-local-v0.1.0.tar.gz` | `961d0152e02b0aa45882b9f5a1bcc83990bb062324f4453ff82cc3c6c8f3feca` |
| `isosui-hosted-v0.1.0.tar.gz` | `c95152eccd5d668dacc4be2e10675c0ad3537d2221ba79acc4dc4db41a449799` |

Each archive's `manifest.json` declares `schema_version: 1`, `version: "0.1.0"`, the exact source commit above, `api_schema_version: 1`, and its own `build_target` (`local` or `hosted`). The downloaded `SHA256SUMS` passed `sha256sum -c SHA256SUMS`. The local verification transcript and downloaded assets are under `/home/operator/q04t/isosui-published-record/`.

Island-web's reviewed [pin PR #138](https://github.com/Island-Team/Island-web/pull/138) merged as `a5c20eb527634c9e8be3aed80a40278e15ffbb02`, pinning this hosted digest, tag, commit and API schema. Its [deployment run 41711195077](https://github.com/Island-Team/Island-web/actions/runs/41711195077) succeeded. That workflow result establishes website deployment; it does not by itself prove a live browser completed the loopback owner first-use path. The Q03 and merged-head Q04 browser replays used a simulated hosted Origin on loopback. The actual public HTTPS browser and cross-owner release replay remain separate qualification evidence.
