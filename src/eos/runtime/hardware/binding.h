/// Parses profile-selected device bindings. Geometry belongs in the robot URDF;
/// invalid or unresolved values throw with file context.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "eos/hardware/backend.h"

namespace eos::runtime::hardware_runtime {

/// Omitted robot IDs are generated at admission; names remain stable URI names.
struct RobotIdentity {
  std::string id;                          ///< configured binding ID or generated UUIDv7
  std::string name{"robot-0"};             ///< fleet-unique display name
  std::optional<std::string> model;        ///< robot model name
  std::optional<std::string> description;  ///< robot-specific description (`robot_desc`)
  std::optional<std::string> vendor;       ///< vendor / integrator
};

/// Planar footprint from `[device.part]`.
struct BasePartConfig {
  double footprint_length_m{0.5};  ///< meters, longitudinal extent
  double footprint_width_m{0.3};   ///< meters, lateral extent
};

/// Gimbal geometry from `[device.part]`.
struct GimbalPartConfig {
  double fov_rad{1.4};  ///< horizontal field of view, radians
  double range_m{5.0};  ///< detection range, meters
};

/// Camera geometry from `[device.part]`.
struct CameraPartConfig {
  std::optional<double> fov_rad{1.4};  ///< horizontal field of view, radians; null if unknown
};

/// Up to six link lengths; omitted trailing TOML values become nullopt.
struct ArmPartConfig {
  std::array<std::optional<double>, 6> link_lengths_m{std::optional<double>(0.18),
                                                      std::optional<double>(0.16),
                                                      std::optional<double>(0.12),
                                                      std::nullopt,
                                                      std::nullopt,
                                                      std::nullopt};
  double mount_x_m{0.0};  ///< arm mount offset on the base, meters
  double mount_y_m{0.0};  ///< arm mount offset on the base, meters
  /// Device-specific target used only by arm.init, in configured joint order.
  std::vector<double> initial_joint_positions_rad;
};

/// Privileged emergency primitives are implicit, never declarable.
enum class PrimitiveTier { kStandard, kOperator, kPrivileged };

/// Runtime management kind of a built-in primitive.
enum class PrimitiveKind { kDirect, kAdvanced };

/// Open bounds are allowed; arrays apply the interval to each item.
struct NumericRange {
  std::optional<double> minimum;
  std::optional<double> maximum;
  bool minimum_exclusive{false};
  bool maximum_exclusive{false};

  bool Contains(double value) const;
};

/// Drives both capability advertisement and dispatch admission.
struct PrimitiveDeclaration {
  std::string name;         ///< dotted primitive name, e.g. "arm.movej"
  std::string version;      ///< exact SemVer primitive contract version
  std::string description;  ///< device-specific purpose; may be empty
  /// Exact enabled model required by an advanced primitive. Empty for direct
  /// primitives. The runtime validates the model capability before opening
  /// northbound listeners.
  std::string model_id;
  PrimitiveTier tier{PrimitiveTier::kStandard};
  std::vector<std::string> references;  ///< admitted motion references: relative and/or absolute
  std::optional<std::string> absolute_frame;   ///< required frame for absolute spatial targets
  std::map<std::string, NumericRange> limits;  ///< hardware-dependent argument ranges
};

/// Shared primitive metadata for admission, discovery, and dispatch.
struct PrimitiveSpec {
  const char* name;
  const char* version;
  const char* device_class;
  PrimitiveTier tier;
  PrimitiveKind kind;
  /// Required internal model capability for advanced primitives; empty for
  /// direct primitives. This is a configuration/admission label, not a
  /// northbound field.
  const char* model_capability;
  const char* description;
};

const PrimitiveSpec* PrimitiveSpecOf(std::string_view name, std::string_view version);
bool BackendCanExecutePrimitive(const PrimitiveSpec& primitive, std::string_view backend_type);

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

/// `[device.backend.aplay]`: Linux ALSA playback settings.
struct AplayDeviceConfig {
  std::string alsa_device{"default"};                ///< ALSA PCM route passed to aplay -D
  std::uint64_t max_stream_bytes{16 * 1024 * 1024};  ///< atomic audio asset bound
};

/// `[device.backend.alsa]`: Linux ALSA capture settings. Every key is optional
/// because the defaults are the audio contracts' own: 16 kHz mono, 20 ms.
struct AlsaMicDeviceConfig {
  std::string alsa_device{"default"};   ///< ALSA PCM route passed to arecord -D
  std::string frame_id{"mic"};          ///< robot-description mounting frame
  std::uint32_t sample_rate_hz{16000};  ///< capture rate
  std::uint16_t channels{1};            ///< interleaved channel count
  std::uint32_t window_frames{320};     ///< frames per published window
  std::uint32_t read_timeout_ms{2000};  ///< bound on one stalled window read
};

/// `[device.backend.v4l2]`: Linux Video4Linux2 capture settings. Every key is
/// optional; the defaults are a 640x480 MJPEG stream at 30 fps.
struct V4l2CamDeviceConfig {
  std::string video_device{"/dev/video0"};         ///< capture device node
  std::string frame_id{"camera"};                  ///< robot-description optical frame
  std::uint32_t width_px{640};                     ///< requested capture width
  std::uint32_t height_px{480};                    ///< requested capture height
  std::uint32_t fps{30};                           ///< requested frame rate
  std::uint32_t read_timeout_ms{2000};             ///< bound on one stalled frame read
  std::uint64_t max_frame_bytes{4 * 1024 * 1024};  ///< accepted encoded-frame bound
};

/// Static per-device configuration: identity, the kind-specific tables the
/// class pulls in, and the device-level operational envelope. The
/// kind-specific `[device.part]` scalars land in the member matching the
/// device's class — populated with documented defaults even when the table is
/// omitted, absent (std::nullopt) for every other class; classes without keys
/// today (mic, gripper, speaker, sim) keep all members nullopt. Unknown keys
/// fail loudly; new classes extend the parser's known-key table, never by
/// ignoring unknown keys.
struct PartConfig {
  std::string kind;                         ///< device-class string, e.g. "base" or "gimbal"
  std::string part_id{"default"};           ///< logical part within the device
  std::optional<std::string> description;   ///< human note, absent when omitted
  std::vector<std::string> tags;            ///< free-form grouping labels
  std::optional<BasePartConfig> base;       ///< set when class is "base"
  std::optional<GimbalPartConfig> gimbal;   ///< set when class is "gimbal"
  std::optional<CameraPartConfig> camera;   ///< set when class is "cam"
  std::optional<ArmPartConfig> arm;         ///< set when class is "arm"
  std::vector<std::string> joint_names;     ///< ordered URDF joint names (arm/gimbal channel order)
  std::optional<std::string> end_effector;  ///< device-level, free-form or "none"
  std::vector<PrimitiveDeclaration> primitives;       ///< [[device.primitive]]
  std::optional<Ros2DeviceConfig> ros2;               ///< set when backend type is "ros2"
  std::optional<AplayDeviceConfig> aplay;             ///< set when backend type is "aplay"
  std::optional<AlsaMicDeviceConfig> alsa;            ///< set when backend type is "alsa"
  std::optional<V4l2CamDeviceConfig> v4l2;            ///< set when backend type is "v4l2"
};

/// One `[[device]]` entry: the class/transport split plus configured feed
/// rates checked against the device's DeclareFeeds() result at startup.
struct DeviceBinding {
  std::string backend_type;             ///< transport: demo | ros2 | aplay | alsa | v4l2
  std::string device_id;                ///< generated `<class>-<index>` handle after admission
  PartConfig part;                      ///< identity, class, and static config
  hardware::FeedDeclarations envelope;  ///< [device.feeds.*] admission envelope
};

/// A parsed binding profile: the devices it declares, the whole-body
/// identity, and the path it came from, kept for factory and admission error
/// messages.
struct Binding {
  std::string path;                    ///< file the binding was parsed from
  std::string profile;                 ///< selected profile name
  RobotIdentity robot;                 ///< [profile.<name>.robot], may be absent
  std::vector<DeviceBinding> devices;  ///< the [[device]] entries
};

/// True when `primitive` names a device-level primitive of the known catalog (the
/// emergency set is implicit and never declarable).
bool IsKnownDevicePrimitive(std::string_view primitive, std::string_view version);

/// Parses binding text. `path` names the file in error messages. `profile`
/// must be non-empty (the [mode] section makes it required) and selects the
/// `[profile.<name>]` table parsed as the binding. Throws
/// std::runtime_error naming the file and the problem when the profile is
/// absent, the text matches no supported shape, or any required value is
/// missing or has the wrong type.
Binding ParseBinding(std::string_view text, std::string path, const std::string& profile);

/// Reads and parses a binding file, selecting `profile`. Throws
/// std::runtime_error naming the path when the file cannot be read or
/// parsed.
Binding LoadBinding(const std::string& path, const std::string& profile);

}  // namespace eos::runtime::hardware_runtime
