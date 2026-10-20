# ROS 2 后端

[English](../../../../../docs/eos/develop/ext/backends/ros2.md)

[扩展与后端总览](../README_cn.md)定义共享后端边界。可选的 ROS 2 适配器将配置的 ROS topic 和 service 映射为 EOS 设备 feed 与 command；它不会改变规范资源或 primitive 的语义。

仅当 EOS 设备类、ROS 映射、必要 endpoint、消息类型和单位兼容时，配置的设备才会被准入。未知设备类、缺少必要映射、名称格式错误、消息类型不兼容和不受支持的 command 会导致准入或单次操作被拒绝，而不会采用猜测的映射。

传入消息会连同其源时间和声明单位一起转换为后端无关的 sample。独立到达的 topic 各自保留独立的 freshness。需要成对或完整状态的资源只有在每个必要成员都已到达后才可用；stale、过大、格式错误或乱序的数据不会作为新鲜的完整 sample 呈现。ROS 未提供值时，可选字段保持缺失。未知的 gripper force 或其他未测量的量保持未知，而不会变为零。

Manipulator binding 可选择把 `arm_status` 映射到 `std_msgs/String` topic。 EOS 仅在版本化 arm-status envelope 的时间戳、controller state、有序 joint name 以及所有已提供诊断值均有效时才接受它。controller sample 与 joint、 pose telemetry 分别独立过期。工程单位尚未形成契约的 vendor register 值保留 明确的 `*_raw` 名称；它们不会被投影为 velocity、effort、torque、安培、摄氏度 或伏特。

传出 command 只有在通过 schema、target、range、lease 和 safety 检查后才会经由 dispatch 到达。每个 motion 都携带其持久 `command_id`；后端等待匹配的 `command_result` JSON 响应。传输不可用、中间件拒绝、超时、取消和关闭都会返回明确的终态结果。EOS 不会仅因消息已发布就推断执行成功，也绝不会在结果不明确后重新发布 command。

对于 `arm.pick`，适配器发布一个 `std_msgs/String` JSON envelope，其中携带 `command_id` 和通过 schema 校验的 `obj:*` 标识符。由部署方负责的 receiver 根据其当前 world state 解析该 ID，负责 freshness、pickability、规划与抓取执行，持久去重 command，并在 `command_result` 上报告完成结果。`command_cancel` 只携带完全匹配的 ID；重复取消不得影响后续 command。

`command_result` 是一个 `std_msgs/String` JSON 对象，例如 `{"command_id":"pick-19","status":"succeeded"}`；失败时可以添加 `reason`。接受的 status 值为 `succeeded`、`failed`、`cancelled`、`timed_out` 和 `outcome_unknown`。在 `command_timeout_ms` 时仍缺少结果会变为 `outcome_unknown`，且 EOS 不会重新发布该 command。

ROS 2 gimbal 可以显式映射到已准入 ROS 2 arm 的两个关节。其 `gimbal_joint_map` 指定 arm part 以及互不相同的 pan 和 tilt 关节。gimbal 必须逐项重复 arm 的有序六关节列表，并且 DDS domain、namespace、timeout、joint-state、MoveJ、stop、estop 和 health endpoint 必须完全一致。EOS 只有在收到新鲜、完整的 joint sample 后才接受 gimbal command；它会保留四个未映射关节的测量位置，替换映射关节的位置，然后发布一个完整的 MoveJ target。此模式有意共享同一个外部 arm controller；它不会创建第二套低层 actuator protocol。

heartbeat 或 endpoint 丢失会将受影响的设备标记为不健康，而不会编造替代 telemetry。stop、lease 到期、断开连接和进程关闭会请求映射中最安全的归零或 halt 操作；无法交付该操作仍然可观测，并且不会清除 emergency state。

仅在启用 ROS 2 支持时才构建该适配器。验证要求包括转换与故障映射的单元测试或 stub 集成检查，以及用 hardware-in-loop 或具有代表性的 ROS graph 检查 topic/service 兼容性、freshness、取消和关闭行为。通用构建和测试命令见 [EOS README](../README_cn.md)。

## 站点本地 profile 示例

仓库不提供 `live.toml` profile。部署方可在自己的配置根目录中创建被 Git 忽略的站点本地 ROS 2 arm binding。它应使用稳定的 `robot_id`、fleet 内唯一的 `robot_name`、`robot_desc`，以及每个设备的 description。以下说明仅假设存在 `arm-0`，其六个有序关节命名为 `joint_1` 至 `joint_6`；这不是完整的机器人 模板，也不提供 base、gimbal、camera、microphone、gripper 或 speaker。

### Primitive 表面

仅声明 `arm.movej`。payload 必须严格包含六个有限的 `joint_positions_rad` 值，顺序与配置的关节顺序一致，每个值均在 `[-3.14, 3.14]` 内。该 profile 未声明 `speed_rad_s` 限制，因此调用方必须省略这个可选字段，并允许 ROS 2 端选择默认值。

`arm.init` 不受支持，因为没有相应声明或初始关节 pose。即使配置了 pose telemetry，`arm.movel` 也不受支持。所有 base、gimbal、gripper 和 speaker primitive 均不受支持，因为对应的目标设备不存在。`motion.stop` 对 `arm-0` 是隐式的；`safety.estop` 是隐式且作用于整个进程的，会影响唯一已准入的 arm。

### ROS 2 数据与发布条件

该 arm 使用 DDS domain 0 和 namespace `/eos`。joint state 从 `/robot/arm/joint_states` 到达；pose telemetry 从 `/robot/arm/pose` 到达；joint command 发布到 `/robot/arm/movej`；终态结果从 `/robot/arm/command_result` 到达；定向取消发布到 `/robot/arm/command_cancel`；stop 和 estop 发布到 `/robot/arm/stop` 和 `/robot/arm/estop`。超过 1500 ms 的 telemetry 即为 stale。proprioception 和 health 的发布频率上限都是 10 Hz；未配置显式的 ROS health-heartbeat topic，因此 health 由适配器与 telemetry 状态得出，而不是来自独立 heartbeat。

缺少或不兼容的必要 endpoint 定义会导致启动被拒绝；未编译 ROS 2 支持时，该 profile 也会被拒绝。调用时要求 ROS 2 ready、存在活动的 command subscriber、payload 有效且 runtime 未锁存。普通 `arm.movej`、`arm.movel` 与 `gimbal.motion` 携带 command ID，并等待部署节点返回关联的终态响应。Stream 请求在现有 array correlation label 后附加 `;stream=true`，发布一次后立即返回 accepted，且不注册终态等待；默认请求保持原有 label 格式。Stream ID 的迟到 result 会被忽略。Graph-edge adapter 必须把 stream 模式解释为可合并的目标更新，而非普通 blocking command 的重叠调用。该节点必须持久去重请求，且绝不能对重复 ID 再次执行动作。传输丢失、stale telemetry、没有 subscriber、超时、stop、estop 和关闭都保持为明确的终态条件。

请以已提交的 `etc/eos/backends/demo.toml` 作为 profile 结构示例，再应用上文的 ROS 2 mapping 与 admission 规则。
