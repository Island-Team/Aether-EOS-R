#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace eos::backends::config {
/// Only standard ROS 2 topic endpoints are implemented.
struct Ros2Endpoint {
  std::string kind;     ///< topic (implemented) | action | service (parsed, rejected)
  std::string name;     ///< ROS graph name, e.g. "/joint_states"
  std::string message;  ///< standard message type, e.g. "sensor_msgs/JointState"
};

/// Explicit mapping from a logical two-axis gimbal to joints owned by one arm.
struct Ros2GimbalJointMap {
  std::string arm_part_id;
  std::string pan_joint;
  std::string tilt_joint;
  int tilt_direction{1};
};

/// Missing ROS 2 endpoints fail parsing when a declared command needs them.
struct Ros2DeviceConfig {
  std::uint64_t domain_id{0};                ///< DDS domain shared with the ROS graph
  std::string node_namespace;                ///< side-node namespace, e.g. "/eos"
  std::string base_frame{"base_link"};       ///< Cartesian frame of the pose telemetry
  std::uint64_t telemetry_timeout_ms{1000};  ///< health: telemetry older than this is a fault
  std::uint64_t command_timeout_ms{30000};   ///< terminal result deadline; never retried
  std::uint64_t pick_timeout_ms{0};          ///< arm.pick deadline; 0 inherits command timeout
  Ros2Endpoint joint_states;                 ///< sensor_msgs/JointState topic (required)
  Ros2Endpoint pose_query;       ///< geometry_msgs/PoseStamped telemetry topic (required)
  Ros2Endpoint arm_status;       ///< optional std_msgs/String normalized controller status JSON
  Ros2Endpoint stop;             ///< std_msgs/Empty topic (required)
  Ros2Endpoint estop;            ///< std_msgs/Empty topic (required)
  Ros2Endpoint movej;            ///< SI-unit arm.init/arm.movej Float64MultiArray topic
  Ros2Endpoint movel;            ///< Cartesian arm.movel Float64MultiArray topic
  Ros2Endpoint pick;             ///< arm.pick typed object ID std_msgs/String topic
  Ros2Endpoint command_result;   ///< correlated terminal std_msgs/String JSON result
  Ros2Endpoint command_cancel;   ///< correlated command_id std_msgs/String cancellation
  Ros2Endpoint gripper_command;  ///< v1 [action, force|-1], v2 adds [opening|-1, speed|-1]
  Ros2Endpoint gripper_state;    ///< [position_pct, state, force_n|-1] Float64MultiArray topic
  Ros2Endpoint image;            ///< sensor_msgs/CompressedImage JPEG topic
  Ros2Endpoint depth_image;      ///< RGB-aligned sensor_msgs/Image 16UC1 topic
  Ros2Endpoint health;           ///< optional std_msgs/Bool adapter heartbeat
  std::optional<Ros2GimbalJointMap> gimbal_joint_map;
  std::uint64_t max_image_bytes{4 * 1024 * 1024};  ///< hard bound before copying a JPEG
  double depth_scale{0.001};                       ///< raw 16UC1 units to metres
};


}  // namespace eos::backends::config
