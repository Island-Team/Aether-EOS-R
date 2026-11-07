# Q03 IsosUI owner evidence

[中文](../../docs_cn/isosui/q03-evidence_cn.md) · [Design](q03-qualification.md)

**Status:** The reviewed IsosUI code topic and isolated browser baseline pass on selected source 90579a9. The sealed Q03 final-17 selected replay passes on that source, and Q04 final-5 passed on merged IsosUI master with independent archive review. IsosUI [`v0.1.0`](interfaces/releases.md#published-v010-evidence) is published, and Island-web deployed its pinned hosted archive. The [approved published-asset run-7](https://github.com/Island-Team/Island-eos/blob/refactor%28v2%29/runtime-main/docs/island/q04-merged-qualification.md) completed the Q04/M5 cross-owner release gate for its stated scope; live public HTTPS browser first use remains open. Earlier incomplete browser attempts remain diagnostic evidence.

## Source and environment

- Selected code and bundle source: IsosUI 90579a9e86033f4180d81d93a212e7b8a61c1850, based on fetched master 920995788d8e29dd6ab594d31278033bb791cfeb. The code topic has since been squash merged as master 4c745bf92f06cea3e2b10f5038817e8efba8ca06. The Q03 evidence document was based on that master commit; its documentation HEAD did not create a new bundle identity. Q04 later verified the merged master identity recorded below.
- The local and hosted manifests report source_commit 90579a9e86033f4180d81d93a212e7b8a61c1850, version 0.1.0, IsOS API schema 1, and their respective targets. Manifest SHA-256: local ef50ab9a7aef0a366a0e6946c2cec9e0ee4518b78e265abc753a27e7a42a129d; hosted d8f4d5615bc1eebcea9472abac4be5dbde7a7d4b579fed2099307e289cf4ba36.
- Fixture manifest SHA-256: 950b135196c6b7e54cb0fe3dcd5fe04e94117698ad187e3383ebfe3590c9ec08. Lockfile SHA-256: 922c376224fb6814252b074f07e93b9b0d701e19fbafe262b9881f59992ac4e9.
- Linux 7.0.0-38-generic x86_64, 12th Gen Intel Core i5-12490F, Firefox 157.0.1, geckodriver 0.37.1, Python 3.12.3, Selenium 4.38.0, Node 22.23.2, npm 10.9.9; browser viewport 1280 × 800. The raw benchmark reports the exact OS and CPU strings.

## Reproduction and gates

Run from the selected code topic at 90579a9 with Node 22/npm 10 and Selenium 4.38.0 on PYTHONPATH. The complete output and raw files are retained locally under ignored logs/test/20261103-q03-ui-process-reset/ and logs/test/20261103-q03-ui-90579a9/.

    npm run typecheck
    npm test
    npm run build
    node scripts/verify-bundles.mjs
    python3 -m unittest tests/q03_browser_benchmark_test.py -v
    python3 tests/fixtures/q03_browser_benchmark/v1/check.py
    git diff --check
    PYTHONPATH=/tmp/q03-ui-py python3 scripts/q03_browser_benchmark.py --target local --warmup 100 --samples 1000 --repetitions 3 --output logs/test/20261103-q03-ui-90579a9/browser-local-90579a9.json
    PYTHONPATH=/tmp/q03-ui-py python3 scripts/q03_browser_benchmark.py --target hosted --warmup 100 --samples 1000 --repetitions 3 --output logs/test/20261103-q03-ui-90579a9/browser-hosted-90579a9.json

TypeScript passed; Vitest passed 191 tests in 41 files; both production builds and manifest verification passed; the focused Python harness passed 10 tests; the fixed fixture checker and diff check passed. A three-repetition lifecycle smoke per target (one warm-up and two measured samples per case) passed with distinct sessions, driver PIDs and five asset hashes in every repetition. The reviewed harness change closes two evidence-loss paths: browser start or prior-session teardown failure, and asset verification failure after a completed case, now write a failure checkpoint before a run is published.

The fixed fixture has 19 graph nodes, 10 wires, one active base binding, 5 ms stub response delay and a 2 s interaction deadline. The runner acquires isolated loopback ports 16000, 16001 and 16020 before browser launch. Each repetition uses a fresh Firefox and geckodriver session and verifies its local or hosted bundle and observed asset hashes. Each Control sample submits exactly one uniquely identified synthetic primitive request to the EOS stub and displays the matching terminal result after a same-run, same-revision check. No live owner or physical actuator receives these requests. Throughput divides completed operations by the sum of measured browser monotonic intervals; reset and warm-up wall time are recorded separately. Median uses the midpoint; p99 uses nearest rank.

## Browser baseline

Both full runners exited 0. Each target completed three repetitions of 100 warm-ups plus 1,000 measured Graph and 1,000 measured Control operations per repetition: 6,000 measured operations per target, zero failures. Each Control repetition recorded 1,100 unique synthetic POST IDs including warm-ups. Each target used three distinct browser sessions and geckodriver PIDs and verified five JS asset digests in each repetition, including lazy chunks. The raw files contain every sample latency, outcome, response byte size and SHA-256.

| Target | Run | Case | Complete / fail | Operations/s | Median ms | p99 ms |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| Local | 1 | Graph | 1,000 / 0 | 31.11 | 30 | 49 |
| Local | 1 | Control | 1,000 / 0 | 5.46 | 179 | 262 |
| Local | 2 | Graph | 1,000 / 0 | 32.07 | 29.5 | 48 |
| Local | 2 | Control | 1,000 / 0 | 5.15 | 180 | 280 |
| Local | 3 | Graph | 1,000 / 0 | 31.90 | 30 | 48 |
| Local | 3 | Control | 1,000 / 0 | 5.43 | 178 | 245 |
| Local | Pooled | Graph | 3,000 / 0 | 31.69 | 30 | 48 |
| Local | Pooled | Control | 3,000 / 0 | 5.35 | 179 | 280 |
| Hosted | 1 | Graph | 1,000 / 0 | 29.84 | 30 | 54 |
| Hosted | 1 | Control | 1,000 / 0 | 5.21 | 180 | 278 |
| Hosted | 2 | Graph | 1,000 / 0 | 27.38 | 32 | 60 |
| Hosted | 2 | Control | 1,000 / 0 | 4.27 | 230 | 281 |
| Hosted | 3 | Graph | 1,000 / 0 | 29.19 | 30 | 57 |
| Hosted | 3 | Control | 1,000 / 0 | 4.84 | 195 | 280 |
| Hosted | Pooled | Graph | 3,000 / 0 | 28.76 | 30 | 57 |
| Hosted | Pooled | Control | 3,000 / 0 | 4.74 | 227 | 280 |

Raw JSON SHA-256: local 4960fc8535ecccbf9125603585668d4251a8d7a76080a062323eb4b4bac724de; hosted b7a092f27db974fad820e23a9b1a08c1ac0e5521e5b96e19e7c8ec8c251d3634.

The older long-lived-browser attempt at fecafe9 completed a full local retry but its hosted retry failed in repetition 3 Control warm-up before any EOS time or primitive POST. A separate earlier local attempt failed in repetition 2 Control before submit. Their raw checkpoints and logs remain under ignored logs/test/20261103-q03-ui-fecafe9/. They are excluded from this complete exact-head baseline and are not treated as successful runs. Firefox memory reached roughly 4–6 GiB during repeated navigation, even in fresh sessions; this is an observed benchmark environment cost and no performance target is asserted.

## Selected installed replay

The sealed archive is retained under EOS ignored `logs/test/20261103-q03-selected-final-15/live/`. Its frozen selection SHA-256 is `116aef2846750a96275645e4dec3021ad716ce6fb80b05c087a2ce377a74b990`; the external frozen build receipt is `977293e097779f636cb70bde9f3d5657bc9351b0b9f67534ca7beb41a97ef915`; the case build receipt is `20b2dc444d4f90ca1e4cf4b8a2404893c53cd20de19a090ce02debf0c539ab5d`. The archive verifier reports `Q01_FULL_VERIFIED` for camera, G06/R03, P01/P03/U01, P07/U03, R06/A03 and explicit migration. The P01/P03/U01 case includes a ghost-device negative path with unchanged graph bytes after rejected Apply.

In the visible selected local UI, one active `base.velocity@1.0.0` binding sent `x_m_per_s: 0.1` with a 250 ms lease. The browser showed Sending then Completed for command `ui-1fb70aba-7a11-4ca7-9b7e-738093ee07b3`; the authorized EOS bridge result read returned HTTP 200 and Completed for that same ID and active revision `4460cbb317cbaf76394936d6dc790db88466129b31a2bb1c519f4e2ce3c944e4`. The selected Vision panel rendered a 640 × 480 JPEG whose verified SHA-256 was `f3ff9f047ca4b88a0ad97d79f6097e68f17ca62a6dc160efdb1337a28961ce2b`, with the run, gate, source, stream and revision attested in the archive. Owner fault/restart and safe Stop were verified by the cross-repository replay, not by the isolated browser benchmark above.

The hosted-route check used a **simulated hosted Origin** on loopback with forwarding to the approved `https://os-island.com` Origin. A direct synthetic loopback Origin received 403 without an `Access-Control-Allow-Origin` header from IsOS and EOS. This is evidence for the selected hosted-route contract, not a public HTTPS deployment. At the final-15 gate, Q04 merged-head replay had not yet run.

## Final selected and merged replay, 2026-11-04

The later sealed Q03 `20261103-q03-selected-final-17` archive retains IsosUI selected source `90579a9e86033f4180d81d93a212e7b8a61c1850` and frozen selection SHA-256 `35dd4e5aa4c209bf84935002a4eded756f3eb5a1c147b2fb12bd5d894e46b78a`. The verifier on its exact selected EOS harness at `219f031bae9dbfc9d9f53f62c910b9e235bb2138` returned `Q01_FULL_VERIFIED` for all six case groups. Independent audit output is under EOS `logs/test/20261104-q03-final17-independent-audit/`; a rerun from the later EOS checkout rejected its changed harness digest, so the selected-source verification is the Q03 result. The final-15 observations above remain historical and retain their original command, revision and image digests.

The sealed Q04 `20261103-q04-merged-home-final-5` archive pins IsosUI `master` at `f0d29848b57ba34e2557cc978fb294912064cc5c` and EOS integration branch `refactor(v2)/runtime-main` at `0bd28b4d2c3ba90a43b81c02ea4e883ef5043db0`; its external selection SHA-256 is `7f5cefa5b6521d42d28b88c916679f2b668ba93fd61bbcbbfed4bb8fcc722b87`. In the actual visible browser, the Graph editor showed the unavailable `ghost-device` and disabled Save and Run; IsOS's owner response identified `mujoco_device_unavailable`. A visible base control submitted `x_m_per_s: 0.1` with a 250 ms lease and retained command ID `ui-cd5f2d76-f184-4fd4-84ce-1f4a31696397` through Sending and Completed. The authorized public bridge result read returned HTTP 200 Completed for that same ID and active revision `351de2c95db402deb772809ec8f9032e37dfdcf2fab13b72f96deea4c8ed3069`. Vision rendered a 640 × 480 JPEG with digest `2415a3115891e19200164708c64f45cedada33b5bddc4e18eb3f3fb0c7d1c7ce`, source `eos/hardware/camera/front`, gate `eos/interface/gate/front-preview`, and generation 95. The archive retains actual screenshots and owner HTTP/event receipts. Its selected verifier returned `Q01_FULL_VERIFIED`, and independent archive review checked 3,630 sealed files with no P1/P2 findings. This replay used a simulated hosted Origin on loopback. The later IsosUI publication and Island-web deployment are recorded in the [release contract](interfaces/releases.md#published-v010-evidence); the later [approved published-asset run-7](https://github.com/Island-Team/Island-eos/blob/refactor%28v2%29/runtime-main/docs/island/q04-merged-qualification.md) completed the Q04/M5 cross-owner release gate for its stated scope; live public HTTPS first use remains open.
