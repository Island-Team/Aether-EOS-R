# Q04 merged-head qualification evidence

[中文](../../docs_cn/island/q04-merged-qualification_cn.md) · [Q03 qualification](q03-qualification.md) · [Cross-repository TODO](implementation-todo.md)

**Status: fifth merged-head archive passed full offline verification; Q04 release work remains open.** Q03's final-17 archive qualifies five selected topic heads. Q04 replays the installed operator path against actual merged integration branches, reviews the complete archive and external receipts, then carries out separate product release gates. The first failed attempt ID is `20261103-q04-merged-final-1`. Its files were copied byte for byte from the temporary checkout to the ignored `/home/ashex/projects/Island-eos/logs/test/20261103-q04-merged-final-1/` archive before that checkout was removed. The source paths recorded inside those files still refer to `/tmp/q04-merged-eos/`.

## Frozen merged inputs

The reviewer-frozen selection for this failed attempt is `/tmp/q04-merged-review-20261103-q04-merged-final-1/selection.json`, SHA-256 `0575c8487f10556605a87844273fa2783e65d6c076374e958d74bdb544b9c1e6`. It uses the existing `selected-q03` verifier profile for this Q04 attempt; the profile name does not make these topic heads. Each owner ref and exact commit in that file is:

| Owner | Integration ref | Selected commit |
| --- | --- | --- |
| EOS | `refs/heads/refactor(v2)/runtime-main` | `3fb3373f4db436c3db861200ef02e1121d8f7a94` |
| IsOS | `refs/heads/master` | `4aa76cd1cc82b63829bbcdbf19336f342574ac8b` |
| EGIA | `refs/heads/master` | `5ca24ecf77ec1c4b4c3fc9eb33fcc8f4d2cfcf82` |
| IsosUI | `refs/heads/master` | `f0d29848b57ba34e2557cc978fb294912064cc5c` |
| MuJoCo | `refs/heads/master` | `c9cb804e977acd585e94d9021e7fe7bcc6a3ce32` |

The rebuilt `island/mujoco-sim@0.1.1` candidate in the frozen selection has release SHA-256 `af08700997448deb4d699694ef4e1faf0b83652f82bc00468ace3731abe5ee49` and archive SHA-256 `746b413b771e869e41929075fc92784cf4aa3fa0c53f2f32ddd2b23cfad95ffc`. Its source commit is the MuJoCo merged `master` commit above. These digests identify candidate input bytes, not a published package.

## First-attempt result and next gate

`main.log` records confirmed `M4_STOP` and `Q01_RESTART_SCOPED_PASS`. The `camera`, `g06-r03`, and `p01-p03-u01` case logs passed. `p07-u03.log` records creation of its case archive; creation alone is not a completed case verdict. `r06-a03.log` ends with an HTTP 422 `PUT node-graph` Apply failure: diagnostic `generated_missing_input`, path `""`, stage `binary`, message `Restore the selected installation and Apply again`. The migration case and full offline verifier were not reached. The precise source is the copied `r06-a03.log` and `main.log` under the archived run path above; raw case files remain under its `live/cases/` tree.

The root filesystem had about 35 MiB free while this attempt ran, and the archive was copied before temporary checkouts were removed. That observation does not establish the cause of the Apply diagnostic. A separate MuJoCo release audit found a metadata/RUNPATH blocker; the focused release fix and a new merged-head selection and replay are required before accepting Q04. This attempt's selection and package digests remain historical input identities and must not be presented as the next run's pins.

The following table describes final-1 only; final-5's accepted result is recorded in its own section below.

| First-attempt evidence | Final-1 result |
| --- | --- |
| Live clean-home replay, six matrix cases, failure/Restart and safe Stop | First attempt incomplete: main path passed, three cases passed, P07/U03 archive created, R06/A03 Apply failed; new full run required. |
| Local and simulated hosted browser observations, command/result and Vision correlation | First-attempt observations exist, but they do not qualify the incomplete matrix; new run and archive review required. Simulated Origin cannot establish public HTTPS deployment. |
| External main and case build receipts and their SHA-256 digests | First-attempt files are diagnostic only; final-5 has separate accepted receipts below. |
| Archive checksum manifest and full offline `tests/verify_q01.py` verdict | Not reached in final-1; final-5 has a separate accepted seal and verifier below. |
| Owner gate, benchmark and bilingual first-use review | Not established by final-1; see owner records and final-5 below. |
| EOS integration-to-`master` reconciliation and selected 0.x product tags | Not established by final-1; still separate Q04 release steps. |

Only a successful verifier against a fresh exact selection, both external receipts and its complete checksum-protected archive can qualify the merged-head replay. The MuJoCo source/package fix prompted the second selection below, which failed before launch for a separate endpoint error. Keep Q04 unchecked until the applicable public snapshot, product release and remote-tag gates also pass.

## Second attempt: merged-home-final-2 (failed before launch)

The new frozen selection SHA-256 is `4da1c95c77dce5fff8757fcb3838ef3f009e252eb19eb75725bf95f3f9b4b47d`, also recorded in `live/q03-selection-sha256.json`. Its five exact integration commits are EOS `c8000bbb6f3f82ca027edba0e129c92a9d3d740a` (`refactor(v2)/runtime-main`), IsOS `4aa76cd1cc82b63829bbcdbf19336f342574ac8b`, EGIA `5ca24ecf77ec1c4b4c3fc9eb33fcc8f4d2cfcf82`, IsosUI `f0d29848b57ba34e2557cc978fb294912064cc5c`, and MuJoCo `04b65882cd11b0aff8ce56473164691dbd662196` (the four sibling refs are `master`). The rebuilt MuJoCo candidate archive SHA-256 is `bb2862b01c3ccab8b68570e766a0ad26c8c5783b3c87cf99225eb0edb7a260f4`; release SHA-256 is `c421039753c24ffc74658d6b1dfe5c9de6a1ed77b64df8fbaee77063da837096`.

The ignored archive is `/home/ashex/projects/Island-eos/logs/test/20261103-q04-merged-home-final-2/`. `live/run.log` records IsOS `vision_endpoint_unavailable: repair private control-grant state before launching`; `main.log` ends after process-status remained HTTP 503 and the running-child readiness condition never became true. Owner investigation identified the inspection Unix socket pathname as 111 bytes, beyond the 107-byte Linux pathname limit. The run did not launch the selected child path, capture a browser result, run the six cases, seal the complete archive or execute the full offline verifier. Its selection and package digests above remain frozen identities for this failed attempt, with no result transferred from final-1.

The coordinated IsOS and EGIA endpoint-owner fix preceded the final-3 selection below. That run still had to record its own external receipts, browser and Vision correlation, all six cases, archive seal and independent verifier. Neither earlier attempt qualifies Q04.

## Third attempt: merged-home-final-3 (failed qualification)

The frozen selection SHA-256 is `124766815290e1eaeeacdd975816ea7c062e3f190b86cc8e0878f423d4676623`, recorded in archived `live/q03-selection-sha256.json`. Its owner commits are EOS `9c0c632ec9ac7d469abef063820620e552c5cbee`, IsOS `76a1d6644ec4e73a0bd47105652d9bbc18a9cae3`, EGIA `cca1ce66dc2481e2473b55325bc453da1b61c44c`, IsosUI `f0d29848b57ba34e2557cc978fb294912064cc5c`, and MuJoCo `04b65882cd11b0aff8ce56473164691dbd662196`. The MuJoCo candidate asset/release digests match final-2 above. The ignored archive is `/home/ashex/projects/Island-eos/logs/test/20261103-q04-merged-home-final-3/`.

This attempt passed the short-endpoint prelaunch boundary and produced truthful live observations. `live/browser-dom-observation.json` records visible IsosUI Control command `ui-b952e23f-a913-4bd9-b2ec-3461e2dcd20f` at `Completed`; `live/browser-result.json` records a matching bridge result with HTTP 200. `live/browser-visible-vision.json` records the Vision panel `Receiving frames`, generation 125, 640×480 and JPEG SHA-256 `8ec10f7a08c0a9b0638c22e05b00302ae975feaeef0743a48e86e8e071be2001`. `main.log` records confirmed `M4_STOP`; `live/reap-proof.json` records `all_absent: true` immediately after Stop, and `live/q01-step-order.json` retains the restart and Stop order.

The qualification failed because the frozen capture helper still constructed the pre-migration Vision socket path in control-grants after IsOS had switched to compact `run/v` endpoints. The capture retries end in `FileNotFoundError`; `FAILED_HARNESS.txt` identifies the stale helper. `main.log` exited 1 at `both hosted editor observations required`; the hosted/browser raw evidence required by the verifier was absent. No six-case matrix or full archive seal was completed. The live UI and Stop observations are diagnostic evidence and do not substitute for those missing verifier inputs.

EOS harness [PR #343](https://github.com/Island-Team/Island-eos/pull/343) merged the path correction to `refactor(v2)/runtime-main` at `f8945d7a`. Final-4 below uses a fresh selection with that correction; its remaining matrix, receipt, seal and verifier gates are tracked separately. Q04 remains unchecked.

## Fourth attempt: merged-home-final-4 (live pass, case verifier failure)

The new frozen selection SHA-256 is `5972e0dd386beb516bbb97f59ccb9b9f195e63b0557e6f0caff80c564a1ab190`, recorded in `live/q03-selection-sha256.json`. It pins EOS `f8945d7a813973f8ba389c78b543f98286d99f61` on `refactor(v2)/runtime-main`, IsOS `76a1d6644ec4e73a0bd47105652d9bbc18a9cae3`, EGIA `cca1ce66dc2481e2473b55325bc453da1b61c44c`, IsosUI `f0d29848b57ba34e2557cc978fb294912064cc5c`, and MuJoCo `04b65882cd11b0aff8ce56473164691dbd662196` on their `master` integration refs. The MuJoCo candidate asset SHA-256 is `bb2862b01c3ccab8b68570e766a0ad26c8c5783b3c87cf99225eb0edb7a260f4`; release SHA-256 is `c421039753c24ffc74658d6b1dfe5c9de6a1ed77b64df8fbaee77063da837096`. The ignored archive is `/home/ashex/projects/Island-eos/logs/test/20261103-q04-merged-home-final-4/`.

`main.log` records confirmed `M4_STOP` and `Q01_RESTART_SCOPED_PASS`. The visible IsosUI Control command `ui-b23866f8-6f37-40e5-a59b-5fd3e892055b` traversed Sending → InProgress → Completed (`live/browser-dom-observation.json`), and `live/browser-result.json` records its HTTP 200 public bridge follow-up with the same command ID and `Completed` status. `live/browser-visible-vision.json` records a rendered 640×480 frame, generation 114, JPEG SHA-256 `2a7c265add6a020a20b9e50509687633c8ce38748301ead5076a9ffdc72bbd98`, and `Receiving frames`. These are live observations from this run, not the full offline verifier verdict.

`camera.log`, `g06-r03.log`, and `p01-p03-u01.log` end with pass markers. `p07-u03.log` records case archive creation, without a completed case verdict. R06/A03 then failed in its EGIA process-restart pytest subprocess: `live/cases/r06-a03/egia-process-restart/pytest.log` says the selected `/home/ashex/projects/q04-review/egia/.venv/bin/python` has `No module named pytest`, and `r06-a03.log` ends at the failed subprocess assertion. Migration, the six-case matrix, external case receipt, checksum seal and independent `tests/verify_q01.py` full verdict were not completed.

The test environment was repaired outside the selected source: `pytest` and `pytest-asyncio` were installed in the EGIA virtual environment, then the expected `../Island-eos/share` sibling was copied from the exact EOS checkout and compared byte for byte. `build/environment-recovery.json` records the commands, dependency versions, 148 copied share files and selection SHA-256; `build/a03-recovery-pytest.log` records seven passes and one warning. This separate check did not retroactively pass the failed frozen R06/A03 case.

An independent reviewer approved a cases-only recovery. The failed case tree and five case logs were moved intact to `/home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/runs/20261103-q04-merged-home-final-4/external/failed-cases-attempt-1/`; its `manifest.json` records SHA-256 entries for 1,010 files. `build/recovery-preflight.log` records the renewed package/source preflight, and `build/recovery-selection-verify.log` verifies all five selected remote refs, tracked UI build inputs and 37 frozen harness hashes.

The clean full six-case rerun reached live R06/A03 behavior and its EGIA process-restart pytest completed seven tests with one warning. The selected EOS case verifier then failed at `tests/verify_q01_r06_a03.py:269`: it asserted `required_libraries` must be empty, while the frozen reviewed MuJoCo release declares a nonempty list. `r06-a03.log` preserves the assertion traceback, and `FAILED_HARNESS.json` records `failed_qualification`, no case receipt, no seal and no full verifier. The prior `pytest` environment gap is resolved; this is a separate selected harness source error. The result does not qualify final-4, even though its live and earlier case observations are retained. Focused EOS verifier [PR #344](https://github.com/Island-Team/Island-eos/pull/344) merged at `0bd28b4d`; final-5 below uses that correction on a fresh frozen selection. Q04 remains unchecked; public release and EOS integration-to-`master` remain separate gates.

## Fifth attempt: merged-home-final-5 (full archive verified)

The frozen selection SHA-256 is `7f5cefa5b6521d42d28b88c916679f2b668ba93fd61bbcbbfed4bb8fcc722b87`, recorded in `live/q03-selection-sha256.json`. It pins EOS `0bd28b4d2c3ba90a43b81c02ea4e883ef5043db0` on `refactor(v2)/runtime-main`, IsOS `76a1d6644ec4e73a0bd47105652d9bbc18a9cae3`, EGIA `cca1ce66dc2481e2473b55325bc453da1b61c44c`, IsosUI `f0d29848b57ba34e2557cc978fb294912064cc5c`, and MuJoCo `04b65882cd11b0aff8ce56473164691dbd662196` on their `master` integration refs. The MuJoCo candidate asset SHA-256 is `bb2862b01c3ccab8b68570e766a0ad26c8c5783b3c87cf99225eb0edb7a260f4`; release SHA-256 is `c421039753c24ffc74658d6b1dfe5c9de6a1ed77b64df8fbaee77063da837096`. `build/selection-verify.log` records all five selected commits and remote refs verified, 103 tracked UI build inputs and 37 harness hashes. The ignored archive is `/home/ashex/projects/Island-eos/logs/test/20261103-q04-merged-home-final-5/`.

The live main and browser capture passed. `main.log` records confirmed `M4_STOP` and `Q01_RESTART_SCOPED_PASS`. The visible IsosUI Control command `ui-cd5f2d76-f184-4fd4-84ce-1f4a31696397` reached `Completed` (`live/browser-dom-observation.json`), and `live/browser-result.json` records a matching public bridge follow-up with HTTP 200 and `Completed`. `live/browser-visible-vision.json` records an actually rendered 640×480 Vision frame, generation 95, JPEG SHA-256 `2415a3115891e19200164708c64f45cedada33b5bddc4e18eb3f3fb0c7d1c7ce`; the screenshot is `live/browser-visible-control-vision.png`.

The six entries of `live/q01-matrix.json` are `camera`, `g06-r03`, `p01-p03-u01`, `p07-u03`, `r06-a03`, and `migration`. Their current-run case files include package/asset and rejected-edge negatives, bounded log cursors, R06/A03 fault and restart, explicit migration and tamper evidence. P01/P03/U01 required a separate selected `--ui-stage` invocation after its owner replay; `p01-p03-u01-ui-stage.log` ends `Q01_P01_P03_U01_REPLAY_PASS` and exit 0. `live/cases/p01-p03-u01/visible-editor.json` and `.png` record the actual browser editor retaining unavailable `ghost-device` and disabling Save and Run; `p01-p03-u01-verify.log` records the negative verifier pass. `camera.log`, `g06-r03.log`, `r06-a03.log` and `migration.log` end with selected case pass markers. The full offline verifier also checks P07/U03 and the case tamper fixtures; `seal-full-verifier.log` and `verify-full-verifier.log` both end `Q01_FULL_VERIFIED`.

The external reviewer files are under `/home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/runs/20261103-q04-merged-home-final-5/external/`. The frozen `selection.json` SHA-256 is `7f5cefa5b6521d42d28b88c916679f2b668ba93fd61bbcbbfed4bb8fcc722b87`. The selected producer's original build receipt SHA-256 was `b428ba0b899683ce4cfb9dfff97c1ead4e198103782e44505c1188faaeca6ed4`; the reviewer wrote an **external-only canonical JSON** receipt with SHA-256 `f20b42a50ca7ce68fb27f057742d1e4cc7951853d50b919c1ac388b151d7d88a`. `build-receipt-canonicalization.json` records semantic equality and that live archive bytes were untouched. The external case receipt SHA-256 is `cbfd7cb46bd4ba934933c356611f309e1d547940250f1f2fecf5cb483bf7195b`. The archive's 3,630-file `live/evidence.sha256.json` manifest has SHA-256 `b3c06413e1c99bbad91c4169c806f853e37be8f7fceb1786558d3ca6ff09e29b`. Independent archive review found no P1/P2 findings.

### Reproduction environment and commands

The final-5 replay used Ubuntu 24.04, MuJoCo 3.16.0, GCC 13.3.0, Node 22.23.4, npm 10.9.9, and the selected EGIA virtual environment with `pytest 9.2.0` and `pytest-asyncio 1.4.1.post1`. All five clean detached checkouts and real binary/build paths were under `/home/ashex/projects/q04-review/`; temporary files used `/home/ashex/q04t` to keep Unix socket paths short. The fixed EGIA `../Island-eos/share` sibling had 148 byte-matched EOS files (file-map SHA-256 `66606a885f81995037ab23e76a3a81e6452d0b0763fbc87354c7b71965399666`). `/home` had 491.6 GiB free at freeze. Preparation logs for MuJoCo CMake/CTest (26/26), IsOS and EOS Cargo builds, EGIA frozen sync/import, and UI `npm ci`, tests (191/191) and both builds are in `q04-home-replay-prep/build-logs/`; final-5 EOS rebuild, package preflight, freeze, five remote-ref/UI/harness checks, and release-view unit tests are in this run's `build/` directory. The selected paths and binary hashes are recorded in `live/selected-inputs.json` and `live/command-argv.json`.

The ignored reviewer recipe is `/home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/replay.sh` (final-5 wrapper SHA-256 `4af0ead664ee2f0d8165f50790be1efb07be5fe2a32d780a9680dc9aca4bf9b4`); its adjacent `freeze_selection.py` and the selected EOS `tests/m4_affected_replay.py`, six `tests/q01_*_replay.py` programs, `tests/q03_case_receipt.py`, and `tests/verify_q01.py` define the stage commands. With the exact five commits and prebuilt package above checked out, the actual stage sequence was:

The selected build inputs can be prepared with these owner commands; preserve the output and hashes before freezing. The reviewed MuJoCo bundle builder writes the `bundle/release.json` and asset paths used below. The final-5 EOS rebuild is recorded in `build/eos.log`.

```sh
export TMPDIR=/home/ashex/q04t TMP=/home/ashex/q04t TEMP=/home/ashex/q04t
export MUJOCO_ROOT=/home/ashex/mujoco-3.16.0-linux-x86_64/mujoco-3.16.0
cd /home/ashex/projects/q04-review/mujoco
cmake -S . -B build-q04 -DMUJOCO_ROOT="$MUJOCO_ROOT"
cmake --build build-q04
ctest --test-dir build-q04 --output-on-failure
python3 tools/build-backend-bundle.py --build-dir build-q04 \
  --output /home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/bundle
cd /home/ashex/projects/q04-review/isos && cargo build --locked --bin isos
cd /home/ashex/projects/q04-review/eos && cargo build --locked --workspace --bins
cd /home/ashex/projects/q04-review/egia && uv sync --frozen
uv pip install --python .venv/bin/python 'pytest==9.2.0' 'pytest-asyncio==1.4.1.post1'
mkdir -p /home/ashex/projects/q04-review/Island-eos
cp -a /home/ashex/projects/q04-review/eos/share /home/ashex/projects/q04-review/Island-eos/share
cd /home/ashex/projects/q04-review/ui && npm ci && npm test && npm run build
```

With the exact five commits and prebuilt package above checked out, the actual stage sequence was:

```sh
export Q04_RUN_ID=20261103-q04-merged-home-final-5
export Q04_RELEASE=/home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/bundle/release.json
export Q04_ASSET=/home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/mujoco-sim-v0.1.1-linux-x86_64.tar.gz
export Q04_TEMP_ROOT=/home/ashex/q04t
recipe=/home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/replay.sh
bash "$recipe" preflight
bash "$recipe" freeze
export Q04_AUTHORIZED=1
bash "$recipe" main       # terminal 1; wait for live/browser-ready.json
bash "$recipe" capture    # terminal 2; observe visible Control and Vision
bash "$recipe" release    # terminal 2; main then completes Stop and reap
bash "$recipe" cases      # terminal 1; leave running while terminal 2 runs --ui-stage below
```

Keep terminal 1's `cases` process running. After its P01 owner stage creates `live/cases/p01-p03-u01`, run the selected `--ui-stage` command below in terminal 2 against that same output. Its browser inspection saved `visible-editor.json` and `.png`; wait for terminal 1 to finish and generate `q01-matrix.json` before the receipt and seal commands. `p01-p03-u01-ui-stage.log` and `p01-p03-u01-verify.log` record the separate pass. The first `main` receipt was copied outside the archive during `capture`; before `receipt`, the reviewer canonicalized only that external copy because the selected verifier requires compact JSON. `build-receipt-canonicalization.json` and the two receipt files preserve the exact byte and semantic comparison.

```sh
cd /home/ashex/projects/q04-review/eos
# Terminal 2 while terminal 1 runs cases:
python3 tests/q01_p01_p03_u01_replay.py \
  --isos-root /home/ashex/projects/q04-review/isos \
  --mujoco-root /home/ashex/projects/q04-review/mujoco \
  --ui-root /home/ashex/projects/q04-review/ui \
  --eos-bin-dir /home/ashex/projects/q04-review/eos/target/debug \
  --asset "$Q04_ASSET" --release "$Q04_RELEASE" \
  --output /home/ashex/projects/Island-eos/logs/test/$Q04_RUN_ID/live/cases/p01-p03-u01 \
  --selection /home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/runs/$Q04_RUN_ID/external/selection.json \
  --expect-selection-sha256 7f5cefa5b6521d42d28b88c916679f2b668ba93fd61bbcbbfed4bb8fcc722b87 \
  --ui-stage
# After terminal 1 has finished cases, continue in terminal 2:
python3 - <<'PY'
import json
from pathlib import Path
p = Path('/home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/runs/20261103-q04-merged-home-final-5/external')
receipt = p / 'q03-build-receipt.json'
receipt.write_text(json.dumps(json.loads(receipt.read_text()), sort_keys=True, separators=(',', ':')) + '\n')
PY
bash "$recipe" receipt
bash "$recipe" seal
bash "$recipe" verify
```

The `main` and `capture` stages require separate terminals and a human visible browser observation. This sequence describes the preserved final-5 run; a new run needs a unique ID, a new selection, and new externally reviewed receipt hashes. `seal-full-verifier.log` and `verify-full-verifier.log` contain the complete commands' output and both `Q01_FULL_VERIFIED` verdicts.

From the selected EOS checkout at `0bd28b4d2c3ba90a43b81c02ea4e883ef5043db0`, reproduce the offline verdict with:

```sh
python3 tests/verify_q01.py /home/ashex/projects/Island-eos/logs/test/20261103-q04-merged-home-final-5/live \
  --expect-selection /home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/runs/20261103-q04-merged-home-final-5/external/selection.json \
  --expect-selection-sha256 7f5cefa5b6521d42d28b88c916679f2b668ba93fd61bbcbbfed4bb8fcc722b87 \
  --expect-build-receipt /home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/runs/20261103-q04-merged-home-final-5/external/q03-build-receipt.json \
  --expect-build-receipt-sha256 f20b42a50ca7ce68fb27f057742d1e4cc7951853d50b919c1ac388b151d7d88a \
  --expect-case-receipt /home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/runs/20261103-q04-merged-home-final-5/external/q03-case-build-receipt.json \
  --expect-case-receipt-sha256 cbfd7cb46bd4ba934933c356611f309e1d547940250f1f2fecf5cb483bf7195b \
  --expect-isos 76a1d6644ec4e73a0bd47105652d9bbc18a9cae3 \
  --expect-eos 0bd28b4d2c3ba90a43b81c02ea4e883ef5043db0 \
  --expect-egia cca1ce66dc2481e2473b55325bc453da1b61c44c \
  --expect-ui f0d29848b57ba34e2557cc978fb294912064cc5c \
  --expect-mujoco 04b65882cd11b0aff8ce56473164691dbd662196
```

The accepted verdict applies to the exact five merged integration commits and inputs above. The hosted Origin check used a local loopback simulation, not a public HTTPS deployment. EOS integration-to-`master`, public EOS/EGIA snapshot gates, IsOS/MuJoCo/UI release reviews and selected 0.x tags remain open; Q04 stays unchecked.
