# v2 硬件节点的 ROS 2 集成（设计说明）

[English](../../../docs/eos/develop/ros2.md) · [v2 硬件开发](hardware_cn.md) · [现有 C++ ROS 2 后端](ext/backends/ros2_cn.md)

本文定义未来通过 CXX 移植 ROS 2 驱动时的目标映射。当前 v2 基础层已有 Rust 的相机、底盘、机械臂类型化接口以及三类节点的 CXX mock，**尚无** v2 ROS 2 驱动。[现有 C++ ROS 2 后端](ext/backends/ros2_cn.md)描述当前运行时，不能当作 v2 适配器已实现的证据。

## 每个节点一个 ROS 2 适配器

统一 `1.0.0` profile 中每个配置的 `camera`、`base` 或 `manipulator` 节点各注册一个适配器。已校验的节点路径、能力和适配器设置提供路由及端点配置，不另设 ROS 2 节点清单。适配器分别拥有 ROS 订阅、发布器或 action client、健康状态与停止能力。相机只有在 RGB 和深度配准到相同坐标系与关联采样时间时才能组合两者；否则以 `depth: None` 报告 RGB，或拒绝错误配对。底盘把里程计映射为平面位姿和速度，只接受声明的 `body_velocity` 或交由控制器处理的 `relative_pose`。机械臂映射完整有序关节集合及可选工具位姿，只接受声明的完整关节目标或交由控制器处理的末端目标。Rust HAL 不做导航或 IK。

ROS 消息、QoS、DDS domain、topic 名称、action 类型和供应商 SDK 类型都留在 C++ 适配器中；返回 Rust 前转换成[类型化 HAL 值](../interfaces/hardware_cn.md)。适配器配置必须声明并校验所需端点与消息类型，不能猜测 topic 或单位。独立可寻址节点不得共享执行端点，除非一个 C++ 所有者统一串行化释放和停止行为。

## 时间、坐标系与安全

- 将 ROS 源时间复制到 `SampleMeta.source_timestamp_ns`，设 `clock = Ros`；接收时由共享 `aether-common::Clock` 设置 `received_monotonic_ns`。只有本地单调时间用于新鲜度和期限；ROS 时间可能暂停或跳变。
- 保留真实 ROS `frame_id`，不要暗中替换成 profile 坐标系。不匹配或不支持的命令坐标系须由适配器拒绝。相机深度须与 RGB 尺寸和标定对齐。保留关节名称顺序，不得用零补齐缺失状态。
- 运动命令只发送一次并附带 `command_id`；终态结果必须与该 ID 严格关联。释放后的传输超时为 `outcome_unknown`，Rust 与适配器均不能自动重发。若重新连接后控制器可能接收重复请求，应由控制器对 ID 去重。
- 对每个执行器，将 `stop_node` 和急停映射到明确的 ROS/控制器停止操作；停止交付失败必须可见。底盘速度有效期是租约，到期、断开和关闭时控制器或适配器须命令零速度。这些是未来移植要求，并非当前 v2 mock 行为。
- 健康与观测按节点隔离。端点丢失只降级对应节点，过期或不完整样本不得标成新鲜。Rust 基础层已在读取错误时使对应节点降级；ROS 2 端口还须在端点故障中验证该行为。

## 启用移植前的验证

用 stub ROS 图测试转换、坐标系、源时间与接收时间、缺失与乱序消息、载荷边界、命令 ID 关联、超时不确定性、停止和断开。随后用代表性的 ROS 图或实体硬件检查实际 QoS 与控制器行为。完整输出写入 `logs/test/<run-id>/`，并记录 ROS 发行版、图结构、适配器编译选项和结果摘要。普通 Rust 工作区检查必须在未安装 ROS 2 的环境也能执行。
