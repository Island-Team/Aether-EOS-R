# Primitive dispatch

[Runtime overview](README.md) describes the integrated dataflow and links the runtime module set.

`src/eos/runtime/dispatch/` is the complete transport-neutral action gate between callers and backends admitted by the hardware registry. `Dispatcher::Execute(name, version, device_id, arguments)` validates the exact primitive identity through the immutable `init::SchemaStore`, resolves and admits exactly one target, enforces device limits and safety state, and performs exactly one matching backend release. It returns `DispatchResult`, whose status is independent of gRPC or HTTP, plus the canonical `primitive.ack` payload for completed dispatches. It does not implement workflows, route sequencing, object selection, or task memory.

Hardware binding owns the shared `PrimitiveSpec` catalog because declarations must be admitted before Dispatcher is constructed. Dispatcher consumes that same catalog and the admitted declarations to project descriptors and execute releases, so device class, kind, tier, backend compatibility, and numeric argument ranges cannot drift. A device primitive must name a non-empty, generated process-unique `device_id` and the target must declare it in `[[device.primitive]]`. `safety.estop` is process-wide and instead requires an empty device ID. Privileged safety primitives are implicit and absent from discovery.

Implementation is split across `dispatcher.cpp` and `common.cpp`, acknowledgement construction in `ack.cpp`, class handlers in `arm.cpp`, `base.cpp`, `gimbal.cpp`, `gripper.cpp`, and `speaker.cpp`, and stop/estop/lease handling in `safety.cpp`. The hardware registry retains sole backend ownership; only Dispatcher is granted the controlled mutable release seam. Northbound, body perception, and skills cannot obtain mutable backend handles.

## Primitive table

| Primitive | Target | Backend release | Arguments |
| --- | --- | --- | --- |
| `base.velocity` | base | `ExecuteBase` | body-frame `linear_x_m_s`, `linear_y_m_s`, `angular_z_rad_s`; integer `valid_for_ms`; all within admitted limits |
| `base.navigate` | base | `ExecuteNavigateRelative` | admitted `reference`; `x_m`, `y_m`, and `yaw_rad`; optional `stream`; absolute calls require the configured `frame_id`, may set map `z_m`, and use fresh base state; deployment adapters own their physical navigation policy. |
| `base.posture` | base | `ExecutePosture` | `posture`: `stand`, `walk`, or `lie` |
| `gimbal.motion` | gimbal | `ExecuteGimbal` | admitted `reference`; `pan_rad`, `tilt_rad`; optional `speed_rad_s` and `stream`; ordinary relative calls resolve from fresh gimbal state, while consecutive streamed relative calls accumulate from the latest accepted stream target |
| `arm.init` | arm | `ExecuteMoveJ` | no call arguments; uses configured `initial_joint_positions_rad` |
| `arm.movej` v2 | arm | `ExecuteMoveJ` | admitted `reference`; complete ordered `joint_positions_rad`; optional `speed_rad_s` and `stream`; relative calls resolve from fresh complete joint state |
| `arm.movel` v2 | arm | `ExecuteMoveL` | admitted `reference`; complete `position_m` XYZ and unit `orientation_quat_xyzw` XYZW; optional `speed_m_s` and `stream`; relative calls compose with fresh tool-pose state |
| `arm.pick` v1 | arm | `ExecutePick` | one schema-valid `obj:*` identifier copied from world/detection; the backend node owns lookup and planning |
| `gripper.motion` | gripper | `ExecuteGripper` | `action`: `open` or `close`; optional `force_n` |
| `speaker.play` v2 | speaker | `ExecuteSpeaker` | `asset://` `uri`; EOS resolves one bounded 16 kHz mono PCM WAV below the configured asset root |
| `speaker.speak` | speaker | TTS then `ExecuteSpeaker` | `text`; optional `language` and `voice`; advanced primitive requiring its exact bound TTS model to be enabled and healthy, with valid PCM WAV output; validated output is stored as a `tts-<uuid>.<ext>` file in the flat launch attachment directory when enabled |
| `motion.stop` | one base, arm, or gimbal | `Clamp` | none |
| `robot.stop` | admitted robot | parallel `StopMotion` on every base, arm, and gimbal | none |
| `safety.estop` | admitted runtime | `Halt` on every device | none |

The operator tier contains all three arm primitives. `[mode.primitives] expose_operator` controls discovery only; an explicitly targeted, schema-valid call remains available for commissioning.

## Per-device terminal admission

`ExecuteCommand` is the authoritative cross-source primitive gate for ActionList jobs, skill host calls, and direct northbound commands. Normal commands for one admitted device enter one FIFO owned by Dispatcher, so independently scheduled callers cannot race the backend or receive a spurious busy rejection merely because an earlier terminal command is still running. The default bound is 64 retained commands per device, including the active front command, and the default maximum queue wait is 30 seconds. `AdmissionOptions` lets composition lower either bound.

The dispatcher reserves and journals the command ID before joining the FIFO. Cancellation while queued removes the command without releasing the backend; an expired wait completes as `timed_out`; and a full FIFO completes as an admission rejection with `rejected.busy`. Each terminal outcome is retained, so an identical command-ID replay returns the exact result and never queues or releases again. `motion.stop`, `robot.stop`, and `safety.estop` bypass normal admission so safety work is not delayed behind ordinary motion.

## Safety lease and acknowledgements

`base.velocity` is the only primitive with dispatcher lease state. Its required validity lease is fail-closed: the terminal command performs a controlled stop when its lease ends or cancellation wins, while `Tick(dt_s)` remains a backup clamp if progress is interrupted. `motion.stop` immediately clamps its targeted motion device and clears that base’s lease. `robot.stop` fans out controlled stops in parallel to every base, arm, and gimbal, waits for every terminal boundary, aggregates per-device results, and clears leases only for confirmed stops. `safety.estop` halts the complete admitted runtime and clears every lease. The halt latch is irreversible, so only stop operations and repeated `safety.estop` calls remain admitted until restart.

```cpp
eos::runtime::dispatch::Dispatcher dispatcher(registry, schemas);
auto result = dispatcher.ExecuteCommand(
    "base.velocity", "1.0.0", "base-0",
    {{"linear_x_m_s", 0.2}, {"linear_y_m_s", 0.0},
     {"angular_z_rad_s", 0.0}, {"valid_for_ms", 250}}, "velocity-42");
dispatcher.Tick(dt_s);
```

Each terminal result payload follows `primitives/actions/ack.v1.json`: `type`, `primitive`, `primitive_version`, `accepted`, `reason`, `reason_code`, `command_id`, `sequence`, and `result`. Top-level `reason` and `reason_code` describe admission rejection only; they remain null after admission, while terminal failure details stay inside `result`. Expected safety or backend refusals are completed calls with `accepted: false`. Unknown primitive name/version pairs and schema-invalid requests use transport-neutral not-found or invalid-argument statuses before release. Bad targets use `rejected.invalid_target`; defensive argument failures use `rejected.invalid_arguments`; an undeclared target primitive uses `rejected.unsupported`; backend refusal uses `rejected.invalid_state`; the runtime latch uses `rejected.estop`; and an exception after validation returns a complete `rejected.internal` acknowledgement.

Before release, the dispatcher fsyncs a process-global command-ID reservation. Identical concurrent calls join, completed duplicates replay the stored response, and conflicting reuse fails without reaching hardware. A prepared record found after restart becomes `outcome_unknown` and is never released again. The dispatcher enforces every numeric interval resolved during registry admission. `references` and `absolute_frame` are binding-owned admission policy: startup rejects missing, duplicate, or inconsistent values, and dispatch rejects a mode or frame not advertised by the target before backend release. The backend remains the final authority for device-limit enforcement, kinematics, trajectories, collision protection, and durable downstream deduplication. The dispatcher resolves reference frames but does not plan trajectories. `arm.pick` is the narrow deployment hook for a ROS 2 planner that accepts an object ID; other higher-level workflows such as dropping and stowing remain externally authored native skills.

## Logging and debugging

The `dispatch` logger records FIFO contention without serializing arguments or acknowledgement bodies. `primitive_admission_queued` and `primitive_admission_acquired` expose expected waiting at debug level; `primitive_admission_rejected` identifies `queue_full`; `primitive_admission_cancelled` and `primitive_admission_timed_out` identify their terminal wait outcomes; and `primitive_backend_cancel_failed` reports a contained backend exception. Safe fields are primitive identity, device ID, command ID, queue depth or limit, bounded wait milliseconds, and a stable reason code. Correlate these fields with ActionList or skill lifecycle logs; never add primitive arguments, results, credentials, model content, or free-form exception text.

## Testing

`eos_dispatch_tests` verifies skill-versus-direct and two-root ActionList contention on one device, strict terminal progress without busy rejection, queued cancellation before backend release, bounded queue capacity and wait deadlines, exact terminal replay, velocity stopping, and the stop/estop bypass behavior. The simulator-backed CTest target remains authoritative for backend release and physical-state assertions.
