# M4 affected-boundary replay

[中文](../../docs_cn/island/m4-affected-replay_cn.md) · [M4 milestone](implementation-milestones.md) · [cross-repository TODO](implementation-todo.md)

This selected clean-home replay checks the merged M4 owner boundaries; it is not the M4 or M5 release gate. IsOS owns saved graph bytes, selected catalog, Apply, child launch and Stop. EOS owns active admission and its journal. EGIA owns the supervised mission and checkpoint. IsosUI invokes one declared primitive through the public bridge. The earlier M3 exact-pin replay in local `logs/test/20261102-m3-exact-merged-replay/qualification.md` covers physical stop and lost-stop behavior.

The source pins are IsOS `2bdf6ea0ce8dbf72ee73e563d0151364c93ff0c0`, EOS `c3aec9247634ff3cbbf3b4ede687c508f2c0f645`, EGIA `475aa567247d6288c40160a2baafc04c8db65856`, and IsosUI `6afd5bdc84a4a3aa2ca73a93342a09d7067d1cc1`. The verified MuJoCo package is `island/mujoco-sim@0.1.1`. `selected-inputs.json` records source and binary hashes. The EOS binary bundle was built for the M3 exact-pin replay; EOS source remained unchanged through this M4 branch point.

The command below records the historical worktree paths used for this run. After those worktrees are removed, substitute checkouts and binaries at the recorded commits and hashes; `selected-inputs.json` is the authoritative input manifest.

From the EOS topic worktree, with exact built inputs and a fresh output directory:

```sh
python3 tests/m4_affected_replay.py \
  --isos-root /tmp/isos-m4-2bdf \
  --eos-root /home/operator/projects/Island-eos-m4-first-use \
  --egia-root /tmp/egia-m4-merged \
  --isos-bin /home/operator/projects/isos/logs/test/20261102-egia-state-root-exact-merge/isos \
  --eos-bin-dir /home/operator/projects/Island-eos/logs/test/20261102-m3-exact-merged-replay/bin \
  --bundle /home/operator/projects/Island-eos/logs/test/20261102-m3-exact-merged-replay/mujoco-bundle \
  --mujoco-root /home/operator/mujoco-3.16.0-linux-x86_64/mujoco-3.16.0 \
  --ui-dist /tmp/isosui-m4-6afd/dist/local \
  --egia-python /tmp/egia-m4-merged/.venv/bin/python \
  --output logs/test/20261102-m4-affected/exact-merged-full-browser-2 \
  --mission --browser-release-file /tmp/m4-exact-merged-browser-release-2 \
  --browser-pause-seconds 1800
python3 tests/verify_m4_affected.py logs/test/20261102-m4-affected/exact-merged-full-browser-2
```

At `browser-ready.json`, rebuild the pinned IsosUI local bundle with `ISOSUI_API_ORIGIN=http://127.0.0.1:<api_port>` and `ISOSUI_EOS_API_ORIGIN=http://127.0.0.1:<bridge_port>`. Open its temporary URL, inspect the selected EGIA node cards and active revision, click the enabled base binding once, and save `browser-dom-observation.json` plus the public `op=result` reply as `browser-result.json`. Only then create the release file so the harness issues robot Stop, IsOS Stop and captures reaped child status. Never log the control grant contents.

The exact merged run exited zero and strict verification returned `M4_AFFECTED_VERIFIED_PASS` in `logs/test/20261102-m4-affected/exact-merged-full-browser-2/verifier-full.log`: two admitted routed evidence edges, 166 owner events, saved/active/overlay revision `a5ca3bf58cb0f4cfca477e78eb4d9b9137ed59e99b0c8ebb612b7578812d3a37`, supervised EGIA mission operation `b1e1efe9-691a-41a4-9c1d-2fb68ee4b49f` with one matching EOS gRPC journal record, distinct browser command `ui-581a75cb-9b5a-4581-a3d6-c0b3744872d7` completed through `private_bridge`, and confirmed selected Stop. Raw graph, overlay, served UI bundle, selected catalog, owner JSONL, statuses and journal hashes are under that run directory. `reap-proof.json` for this completed run is explicitly labeled a post-run PID absence observation; future runs capture it immediately after Stop. A preceding browser attempt timed out before its click and remains failed in `exact-merged-full-browser/`.

The separate `logs/test/20261102-m4-affected/exact-merged-a03-negative-2/` fresh-home replay used the same exact input paths above with `--output logs/test/20261102-m4-affected/exact-merged-a03-negative-2 --mission --recovery-negative` in place of the browser flags. It exited `M4_A03_SELECTED_NEGATIVE_PASS`; its baseline also passed `verify_m4_affected.py --scope a03`, and `python3 tests/verify_m4_recovery.py logs/test/20261102-m4-affected/exact-merged-a03-negative-2` returned `M4_A03_SELECTED_NEGATIVE_VERIFIED_PASS`. It used supervised mission operation `327e855b-d365-4919-b7c7-61e0becd8e2d`: the next IsOS run's grant returned 404 for that old ID and did not change its EOS journal record. With EGIA stopped, the test injected a different missing pending operation `m4-missing-1793609332425473999` and a private 0600 safety latch. The selected next child emitted unresolved-operation and reset-refused events, kept the latch, and submitted no movement for the missing ID. A controlled EGIA child SIGKILL then caused dependent Stop with unknown outcome and reaped children. The injection and old-run lookup are labeled negative evidence in `a03-recovery/` and are never presented as an ordinary first-use path.

The supervised mission and selected Stop are qualified here. A positive same-command EGIA **process restart** remains unqualified at this selected IsOS boundary: IsOS deliberately performs dependent Stop after a managed child exits; a new Run changes run ID and grant. EGIA's owner [process restart proof #98](retired-repository-references.md#ref-022af3ceccb3) covers its own stable-run recovery contract. Q01's full injected failure/restart and the M4 release gate remain open.
