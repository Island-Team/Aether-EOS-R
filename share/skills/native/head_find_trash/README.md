# `head_find_trash`

## Version and purpose

`head_find_trash@1.0.0` is a reentrant native skill (`island.head_find_trash@1.0.0`). It watches head-camera detections and announces stable sightings of four trash labels by playing pre-synthesized WAV assets through `speaker.play`. By default it continues monitoring until cancelled or an error occurs; bounded and single-detection operation are available for workflows.

## Input contract

`robot_name` and `speaker_device_id` are required non-empty strings. `stop_after_first_detection` is an optional boolean (default `false`) that completes successfully after the first stable detection is announced. `search_timeout_ms` is an optional integer: `0` or omission means no deadline, while `1000` through `300000` bounds the search using elapsed steady-clock time. Additional properties are rejected.

## Resources and claims

The skill reads `robots/{robot_name}/detection/head/state` without holding a long-lived device claim. Each snapshot must contain a non-negative integer `source_sequence` and a `detections` array. Eligible entries have `kind:"object"`, label `peel`, `empty_box`, `bottle`, or `paper_ball`, and numeric confidence in `[0.40, 1.0]`. It calls only `speaker.play@1.0.0` with the corresponding asset under `asset://head_find_trash/`. The primitive call acquires the speaker only for the duration of playback, allowing other long-running skills to use the same speaker between announcements.

The announcement WAVs are **local-only and intentionally untracked**. Each deployment must place four 16 kHz mono PCM WAV files named `found_peel.wav`, `found_empty_box.wav`, `found_bottle.wav`, and `found_paper_ball.wav` under `assets/` in this directory. `eos-launch.sh` mirrors them into the EOS asset root (`var/lib/eos/assets/head_find_trash/`) before `eosd` starts, and EOS resolves and validates the assets at dispatch time. A checkout without these files fails the corresponding announcement at dispatch with an asset-resolution rejection.

## Execution flow

1. Emit checkpoint `search.started` at progress `0.1`.
2. Poll every 20 ms, ignoring snapshots whose `source_sequence` is not strictly greater than the last processed sequence.
3. Count consecutive fresh appearances independently per label. The highest-confidence occurrence supplies that label's result confidence; `object_count` counts all eligible supported objects in the frame across all four labels.
4. On the third consecutive frame, emit `trash.found` and call `speaker.play` with `{"uri":<label asset URI>}`. The spoken announcements are `主人，发现果皮啦`, `主人，发现空盒子啦`, `发现空瓶了`, and `主人，发现纸团啦`.
5. Mark that label announced and emit `announcement.completed`. It cannot be announced again until ten consecutive fresh frames omit it. Other labels continue to be evaluated in array order.
6. If `stop_after_first_detection` is true, return the found result after successful playback. Otherwise continue monitoring.
7. If a nonzero search timeout expires before a stable supported object is found, reject with `trash.search_timeout`.

There is no pause signal or retry. Manager cancellation is checked before timeout and each read, and shared primitive support cancels/drains an active playback operation at 50 ms wait boundaries.

## Outcomes

Cancellation returns cancelled. Search timeout completes with `accepted:false`, reason `trash.search_timeout`, and `{"found":false,"object_count":0}`. A rejected playback completes with `accepted:false`, reason `announcement.rejected`, and `found:true`, `label`, `announcement`, `confidence`, and `object_count`. Playback failures use `announcement.failed`, `announcement.timed_out`, or `announcement.outcome_unknown`. Resource and shape failures use `detection.read_failed` and `detection.invalid`. With `stop_after_first_detection:true`, successful playback completes with the corresponding found result; otherwise monitoring continues.

## Tests and limitations

Dedicated tests cover stable detection, label-specific assets, one-shot completion, timeout, schema labels, cancellation, and rejection of unsupported or low-confidence detections. Confidence need not be finite explicitly: non-finite values fail ordinary comparisons differently (notably NaN can pass the range checks), so producers should publish finite confidence. The implementation does not use bbox, depth, object ID, temporal duration, or spatial continuity; consecutive labels alone establish stability.
