# 时间模型

[English](../../../../docs/eos/design/runtime/time.md)

EOS 区分运行时间与事件时间。**单调时间**驱动 deadline、watchdog、接收后的新鲜度、重试、速率限制和 timer。墙钟或 ROS 时间跳变不能延迟停止动作，也不能让过期硬件重新显得健康。**墙钟时间**仅用于日志、持久化和外部系统关联，不用于测量经过时间。**源时间**是可选的传感器事件时间，其 clock domain 必须标明为 `unix`、`steady_boot`、`ros`、`simulation`、`device` 或 `unknown`；仅当 domain 和 clock ID 都相同时才可比较源时间戳。

Runtime composition 创建一个 `eos::lib::SystemClock`，注入核心 timer 和 telemetry 接收时间戳。测试与回放使用 `ManualClock` 和 `ManualTimerService`。ROS adapter 保留 ROS header time 作为源时间，同时独立记录本地单调接收时间。

当 backend 未提供时间戳时，相机模型 pump 合成并明确标记 `steady_boot` 源时间戳。较慢的 pose 和 face 阶段在处理较新检测帧时保留最近完成的值及其来源信息，使语义观测连续。
