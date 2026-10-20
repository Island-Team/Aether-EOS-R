# `navigate_waypoint`

## Version and purpose

`navigate_waypoint@1.0.0` is a reentrant native skill (`island.navigate_waypoint@1.0.0`) that resolves an inline or stored navigation target and passes it unchanged to `base.navigate@1.0.0`.

## Input contract

`robot_name`, `base_device_id`, and `waypoint_id` are required non-empty strings. Optional `target` has no default; when present it must contain only `reference`, optional `frame_id`, `x_m`, `y_m`, and `yaw_rad`. `reference` is `relative` or `absolute`; all three numeric coordinates must be finite. An absolute target requires a non-empty `frame_id`; a relative target must omit `frame_id`. Additional top-level and target properties are rejected.

When `target` is absent, the skill reads `robots/{robot_name}/navigation/waypoints/{waypoint_id}`. The resource value receives the same runtime validation, although resource objects are not constrained by the launch schema before reading.

## Resources, primitive, and claim

- Optional read: `robots/{robot_name}/navigation/waypoints/{waypoint_id}`.
- Primitive: `base.navigate@1.0.0` on `base_device_id`, with the validated target object as its complete payload.
- Claim: exclusive device use through `/base_device_id`.

## Execution flow

1. Read required identifiers.
2. Prefer the inline `target`; otherwise read the named waypoint resource.
3. Validate reference/frame rules and finite `x_m`, `y_m`, and `yaw_rad`.
4. Honor cancellation before motion and emit `navigation.started` at progress `0.1`.
5. Run `base.navigate` asynchronously through shared primitive support.
6. On success, emit `navigation.completed` at `1.0` and return the waypoint ID and resolved target.

There are no pause/resume signals and no retries. Shared primitive support polls at 50 ms, requests cancellation for an active primitive when the execution is cancelled, and waits for a terminal acknowledgement.

## Outcomes

Success returns `{"waypoint_id":"...","target":{...}}`. Primitive rejection returns completed/`accepted:false`, reason `navigation.rejected`, and the same result object. Primitive failures use `navigation.failed`, `navigation.timed_out`, or `navigation.outcome_unknown`; cancellation returns cancelled. A failed resource read reports `waypoint.unavailable`. Invalid resolved targets are invalid-argument outcomes with `waypoint.invalid`.

## Tests and limitations

No dedicated `navigate_waypoint` automated test was present in the audited native test sources. The skill does not normalize frames, constrain distance/yaw ranges beyond finiteness, verify backend support for absolute targets, monitor localization, or retry. `waypoint_id` remains informational when an inline target is supplied.
