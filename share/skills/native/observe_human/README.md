# `observe_human`

`observe_human` scans with the camera gimbal, confirms a human in three distinct camera generations, and tracks the selected person. If the initial observation at the starting pose finds no target, it looks downward to make nearby fallen people visible before beginning the waypoint scan. If the target is lost, the skill resumes the waypoint scan and looks for the same registered name (or the same detection ID for an unidentified fallen person in the legacy skill, or an unidentified person lying down in v2). It does not request rotated pose or face inference.

`observe_human@1.0.0` uses the lying-person selection rules formerly provided by version 2. The earlier selection behavior remains available as `observe_human_legacy@1.0.0` for callers that require it. Both run for 60 seconds by default, exclusively claim the gimbal, and use a minimum person-box confidence of `0.4`.

## Arguments

| Field | Required | Meaning |
| --- | --- | --- |
| `robot_name` | Yes | Robot used in resource URIs. |
| `gimbal_device_id` | Yes | Device used for `gimbal.motion@1.0.0`. |
| `minimum_confidence` | No | Person-box detection threshold in `[0,1]`; defaults to `0.4` for both versions. |
| `target_loss_timeout_ms` | No | Missing-target grace period; defaults to `3000`. |
| `observation_hold_ms` | No | Stationary observation time at the origin and each waypoint, `0` to `10000`; defaults to `1500`. |
| `acquire_timeout_ms` | No | Total execution duration, including scanning and tracking; defaults to `60000`. |
| `reacquire_tilt_rad` | Yes | Deployment-qualified tilt used only to park the gimbal when the duration ends without ever finding a human. |

Additional properties are rejected. The `acquire_timeout_ms` field name is retained for caller compatibility; its limit now applies to the full execution.

## Detection and confirmation

The skill subscribes to `robots/{robot_name}/detection/state`. It reads the latest publication at startup and waits up to 20 ms for each subsequent publication. A unary read is the fallback when subscriptions are unavailable. Each new publication is inspected, including detector, pose/action, and face updates to the current `source_generation`.

When `models.human` is present, the human detector must report `complete` for the current generation. Pending source snapshots can contain detections retained from an earlier frame and do not advance confirmation or tracking. Snapshots without model status retain the legacy behavior.

A target needs a valid human ID, sufficient confidence, a finite bbox inside the image, and either a registered name or the unidentified `fallen` action in the legacy skill. `observe_human` accepts only registered people or people with `pose:"lie"`, whether or not a registered name is available; `action:"fallen"` alone does not qualify an unnamed person. Explicitly unknown identities are excluded after three fresh generations unless their pose is `lie`. During acquisition, `observe_human` requires the bbox center at or below `13/24` of image height. It prefers a lying person, then a registered person; within the same priority tier it keeps the first eligible detection in array order rather than comparing bbox area. The legacy skill retains its original tilt reachability and target-selection rules.

Only distinct, increasing camera generations count toward the three-frame confirmation. The identity must stay the same and adjacent bboxes must overlap with IoU at least `0.5`. A later publication can replace that generation's candidate but cannot count it twice. After target loss, a registered target requires a face observation newer than the loss boundary before confirmation; an unnamed lying target remains locked to its detection ID.

## Scan and tracking

After recording the starting pan and tilt, the skill first observes at that pose for `observation_hold_ms`. If no target is confirmed, it moves to the starting pan and starting tilt minus `0.3` radians, observes there for five seconds, and returns to the starting pose. It then repeatedly visits starting pan minus `pi/4`, starting pan, starting pan plus `pi/4`, and starting pan. Scan moves use absolute targets, `0.1 rad/s`, and `stream:false`. Detection continues during all moves and observation holds. Once a target is confirmed, the active motion is cancelled and drained.

Tracking reads bbox updates and gimbal state, filters image-center error, and sends at most one streaming correction per camera generation at `0.4 rad/s`. Capture-time `source_gimbal` is used when available; current gimbal telemetry is the fallback. The skill samples measured gimbal rates at 10 Hz for its public state. Missing detections stop corrections immediately. If the target does not return within `target_loss_timeout_ms`, waypoint scanning resumes; there is no stationary downward reacquisition step.

At the duration limit, an execution that found a human completes with `{"found":true}`. If no human was ever found, it publishes `phase:"absent"`, parks at the starting pan and `reacquire_tilt_rad`, emits one `state.fact` `{"key":"owner.found","value":false,"sequence":1}`, and completes with `{"found":false}`. Cancellation does not emit the fact.

Public state contains `phase` (`scan`, `confirm`, `track`, `pause`, or `absent`), optional `target_id`, and measured pan, tilt, and combined angular rates when available. `skill.command` accepts `pause` and `resume`. The skill does not publish detection overlays, attachments, checkpoints, or `skill.event` records.
