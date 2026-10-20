# `head_find_trash_navigation_resume`

`head_find_trash_navigation_resume@1.0.0` is a continuous native skill for Remote EOS. It recognizes `peel`, `empty_box`, `bottle`, and `paper_ball`, using the same three-fresh-frame detection threshold and ten-absent-frame rearm rule as `head_find_trash@1.0.0`.

Launch with `robot_name`, `speaker_device_id`, and `base_device_id`. An optional initial `navigation_command_id` and `resume_target` must be supplied together. While the skill runs, send a `navigation.update` signal with both fields to replace them. `navigation_command_id` is the dispatcher command ID of a `base.navigate@1.0.0` operation, not the primitive name or a skill execution ID. `resume_target` is an absolute map target:

For a bounded workflow, supply either `start_relative_target` with finite `x_m`, `y_m`, and `yaw_rad`, or `start_waypoint_id`, instead of an external command binding. A relative target is converted from fresh localized base pose; a waypoint must resolve to an absolute map target. The skill owns the navigation command and completes when it reaches the destination. It monitors and resumes to that same target after trash announcements along the way. `forward_find_and_pick_trash.json` uses the relative mode for a 3 m forward trip, while `competition_three_rooms_navigation.json` uses waypoint mode for each route segment.

```json
{"reference":"absolute","frame_id":"map","x_m":1.0,"y_m":2.0,"yaw_rad":0.0}
```

On a stable sighting, the skill starts `speaker.play@1.0.0` and requests targeted cancellation of the active navigation command. It waits for both terminal results before starting a fresh `base.navigate` to the saved target. Later sightings can interrupt navigation started by this skill. A signal received during navigation changes the destination used at the next interruption.

If no matching active command exists, the skill emits `navigation.warning`, plays the announcement, skips the resume, and continues watching. Other cancellation uncertainty also skips the resume. Playback failure emits a warning but does not prevent a confirmed navigation from resuming. Manager cancellation stops navigation owned by this skill and never restarts motion.

The skill keeps running until manager cancellation. The deployment needs the same four local `head_find_trash` announcement WAV assets, including `found_empty_box.wav`.
