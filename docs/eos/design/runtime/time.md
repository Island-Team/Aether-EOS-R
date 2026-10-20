# Time model

[中文](../../../../docs_cn/eos/design/runtime/time_cn.md)

EOS follows the ROS distinction between operational time and event time.

- Monotonic time drives deadlines, watchdogs, freshness after receipt, retries, rate limits, and timers. Wall or ROS time jumps must not delay a stop or make stale hardware healthy.
- Wall time is metadata for logs, persistence, and correlation with external systems. It is never used to measure elapsed time.
- Source time is optional sensor event time. Its clock domain is explicit: `unix`, `steady_boot`, `ros`, `simulation`, `device`, or `unknown`. Source timestamps are comparable only when their domain and clock ID match.

Runtime composition owns one `eos::lib::SystemClock` and injects it into core timers and telemetry receipt stamping. Tests and replay use `ManualClock` and `ManualTimerService`. ROS adapters preserve ROS header time as source time while recording local monotonic receipt time independently.

Camera model pumps synthesize a steady-boot source timestamp when a backend does not provide one, and label that clock domain explicitly. Slower pose and face stages retain their last completed values and provenance while newer detection frames are processed, preserving continuous semantic observations.
