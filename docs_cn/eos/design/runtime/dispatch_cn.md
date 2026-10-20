# Primitive dispatch

[Runtime overview](README_cn.md) links the integrated runtime module set.

`src/eos/runtime/dispatch/` is the complete transport-neutral action gate between callers and backends admitted by the bridge registry. `Dispatcher::Execute(name, version, device_id, arguments)` validates the exact primitive identity through the immutable `init::SchemaStore`, resolves and admits exactly one target, enforces device limits and safety state, and performs exactly one matching backend release. It returns `DispatchResult`, whose status is independent of gRPC or HTTP, plus the canonical `primitive.ack` payload for completed dispatches. It does not implement workflows, route sequencing, object selection, or task memory.

Bridge binding owns the shared `PrimitiveSpec` catalog because declarations must be admitted before Dispatcher is constructed. Dispatcher consumes that same catalog and the admitted declarations to project descriptors and execute releases, so device class, tier, backend compatibility, and numeric argument ranges cannot drift. A device primitive must name a non-empty, generated process-unique `device_id` and the target must declare it in `[[device.primitive]]`. `safety.estop` is process-wide and instead requires an empty device ID. Privileged safety primitives are implicit and absent from discovery.

Implementation is split across `dispatcher.cpp` and `common.cpp`, acknowledgement construction in `ack.cpp`, class handlers in `arm.cpp`, `base.cpp`, `gimbal.cpp`, `gripper.cpp`, and `speaker.cpp`, and stop/estop/lease handling in `safety.cpp`. Bridge retains sole backend ownership; only Dispatcher is granted the controlled mutable release seam. Gateway, body perception, and skills cannot obtain mutable backend handles.

## Primitive table

| Primitive | Target | Backend release | Arguments |
| --- | --- | --- | --- |
| `base.velocity` | base | `ExecuteBase` | body-frame `linear_x_m_s`, `linear_y_m_s`, `angular_z_rad_s`; integer `valid_for_ms`; all within admitted limits |
| `base.navigate` | base | `ExecuteNavigateRelative` | admitted `reference`; `x_m`、`y_m`、`yaw_rad`；可选 `stream`；absolute 调用需要已配置的 `frame_id`，并由 EOS 根据 fresh base state 解析 |
| `base.posture` | base | `ExecutePosture` | `posture`: `stand`, `walk`, or `lie` |
| `gimbal.motion` | gimbal | `ExecuteGimbal` | admitted `reference`；`pan_rad`、`tilt_rad`；可选 `speed_rad_s` 与 `stream`；relative 调用根据 fresh gimbal state 解析 |
| `arm.init` | arm | `ExecuteMoveJ` | no call arguments; uses configured `initial_joint_positions_rad` |
| `arm.movej` v2 | arm | `ExecuteMoveJ` | admitted `reference`；完整有序 `joint_positions_rad`；可选 `speed_rad_s` 与 `stream`；relative 调用根据 fresh complete joint state 解析 |
| `arm.movel` v2 | arm | `ExecuteMoveL` | admitted `reference`；完整 `position_m` XYZ 与 unit `orientation_quat_xyzw` XYZW；可选 `speed_m_s` 与 `stream`；relative 调用与 fresh tool pose 组合 |
| `gripper.motion` | gripper | `ExecuteGripper` | `action` 为 `open` 或 `close`；可选 `force_n` |
| `speaker.play` v2 | speaker | `ExecuteSpeaker` | `asset://` `uri`；EOS 从配置的 asset root 解析一个有界 16 kHz mono PCM WAV |
| `speaker.speak` | speaker | TTS 后 `ExecuteSpeaker` | `text`；可选 `language` 与 `voice`；需要恰好一个健康 TTS model 及合法 PCM WAV 输出 |
| `motion.stop` | one base, arm, or gimbal | `Clamp` | none |
| `robot.stop` | admitted robot | parallel `StopMotion` on every base, arm, and gimbal | none |
| `safety.estop` | admitted runtime | `Halt` on every device | none |

The operator tier contains all three arm primitives. `[mode.primitives] expose_operator` controls discovery only; an explicitly targeted, schema-valid call remains available for commissioning.

上述四个 reference-aware target primitive 的 `stream` 默认为 `false`。普通调用等待 backend 的关联终态；stream 调用在目标发布后立即返回 accepted，使兼容的 southbound adapter 可以合并后续目标。两种模式都执行相同的 validation、reference resolution、limit、health、stop 与 estop 检查。

## 按设备的终态准入

`ExecuteCommand` 是 ActionList job、skill host call 与直接 northbound command 的权威跨来源 primitive gate。同一 admitted device 的普通 command 进入 Dispatcher 拥有的一条 FIFO，因此独立调度的 caller 不会争用 backend，也不会仅因前一个 terminal command 仍在运行而收到虚假的 busy rejection。默认上限为每个 device 保留 64 个 command（包括 active front command），默认最长排队时间为 30 秒；组合层可通过 `AdmissionOptions` 下调任一限制。

Dispatcher 在进入 FIFO 前保留并 journal command ID。排队期间取消会移除 command 且不 release backend；等待到期完成为 `timed_out`；FIFO 已满则以 `rejected.busy` 完成 admission rejection。每个终态 outcome 都会保留，因此相同 command ID 的 replay 返回完全一致的结果，且不会再次排队或 release。`motion.stop`、`robot.stop` 与 `safety.estop` 绕过普通准入，使 safety work 不会被普通 motion 延迟。

## Safety lease and acknowledgements

`base.velocity` 是唯一具有 dispatcher lease state 的 primitive。required validity lease 采用 fail-closed：lease 结束或 cancellation 胜出时，terminal command 执行 controlled stop；若进度被中断，`Tick(dt_s)` 仍作为备用 clamp。`motion.stop` 立即停止目标 motion device；`robot.stop` 并行停止所有 base、arm 与 gimbal，等待各自终态并聚合 per-device 结果，只清除已确认停止设备的 lease。`safety.estop` halt 整个 admitted runtime 并清除全部 lease。halt latch 不可逆，因此重启前只接纳 stop operation 与重复 `safety.estop`。

```cpp
eos::runtime::dispatch::Dispatcher dispatcher(registry, schemas);
auto result = dispatcher.Execute(
    "base.velocity", "1.0.0", "base-0",
    {{"linear_x_m_s", 0.2}, {"linear_y_m_s", 0.0},
     {"angular_z_rad_s", 0.0}, {"valid_for_ms", 250}});
dispatcher.Tick(dt_s);
```

每个终态结果都遵循 `primitives/actions/ack.v1.json`：`type`、`primitive`、`primitive_version`、`accepted`、`reason`、`reason_code`、`command_id`、`sequence` 与 `result`。顶层 `reason` 和 `reason_code` 只描述准入拒绝；命令一旦准入，这两个字段保持 null，终态失败详情放在 `result` 内。预期的安全或 backend 拒绝是 `accepted: false` 的完整调用结果。未知 primitive name/version 与 schema 无效请求在 release 前返回 transport-neutral not-found 或 invalid-argument。错误 target 使用 `rejected.invalid_target`，防御性参数失败使用 `rejected.invalid_arguments`，未声明 primitive 使用 `rejected.unsupported`，backend 拒绝使用 `rejected.invalid_state`，runtime latch 使用 `rejected.estop`，校验后的异常返回完整 `rejected.internal` acknowledgement。

Dispatcher 强制执行 registry admission 解析出的全部数值区间。`references` 与 `absolute_frame` 是 binding 自有 admission policy：startup 拒绝缺失、重复或不一致的值，dispatch 在 backend release 前拒绝目标未发布的 mode 或 frame。EOS 将 relative joint、Cartesian 和 gimbal 目标解析为 absolute southbound command，并将 absolute base navigation 转成 backend 的 robot-relative goal。Backend 仍是设备限制、kinematics、trajectory 与 collision protection 的最终权威；高层 picking、dropping、stowing 等行为属于基于此稳定 surface 编写的外部 native skills。

## 日志与调试

`dispatch` logger 记录 FIFO contention，但不序列化 argument 或 acknowledgement body。`primitive_admission_queued` 与 `primitive_admission_acquired` 在 debug 级别呈现预期等待；`primitive_admission_rejected` 标识 `queue_full`；`primitive_admission_cancelled` 与 `primitive_admission_timed_out` 标识各自的终态等待结果；`primitive_backend_cancel_failed` 报告被封装的 backend exception。安全字段包括 primitive identity、device ID、command ID、queue depth 或 limit、有界 wait milliseconds 和稳定 reason code。应使用这些字段关联 ActionList 或 skill lifecycle log；不得添加 primitive argument、result、credential、model content 或自由格式 exception text。

## 测试

`eos_dispatch_tests` 验证 skill 与 direct caller、两个同 device ActionList root 之间的 contention，验证无 busy rejection 的严格 terminal progress、backend release 前的 queued cancellation、有界 queue capacity 与 wait deadline、精确 terminal replay、velocity stopping，以及 stop/estop bypass 行为。基于 simulator 的 CTest target 继续对 backend release 与 physical-state assertion 保持权威。
