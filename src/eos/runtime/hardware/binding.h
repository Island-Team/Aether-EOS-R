/// Parses profile-selected device bindings. Geometry belongs in the robot URDF;
/// invalid or unresolved values throw with file context.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "eos/hardware/backend.h"

namespace eos::runtime::hardware_runtime {

class BackendCatalog;
struct BackendSettings;

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
  std::uint64_t camera_frame_limit_bytes{4 * 1024 * 1024};
  std::optional<ArmPartConfig> arm;         ///< set when class is "arm"
  std::vector<std::string> joint_names;     ///< ordered URDF joint names (arm/gimbal channel order)
  std::optional<std::string> end_effector;  ///< device-level, free-form or "none"
  std::vector<PrimitiveDeclaration> primitives;       ///< [[device.primitive]]

};

/// One `[[device]]` entry: the class/transport split plus configured feed
/// rates checked against the device's DeclareFeeds() result at startup.
struct DeviceBinding {
  std::string backend_type;             ///< registered adapter type
  std::string device_id;                ///< generated `<class>-<index>` handle after admission
  PartConfig part;                      ///< identity, class, and static config
  std::shared_ptr<const BackendSettings> backend_settings;
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
Binding ParseBinding(std::string_view text, std::string path, const std::string& profile,
                     const BackendCatalog& catalog);

/// Reads and parses a binding file, selecting `profile`. Throws
/// std::runtime_error naming the path when the file cannot be read or
/// parsed.
Binding LoadBinding(const std::string& path, const std::string& profile,
                    const BackendCatalog& catalog);

}  // namespace eos::runtime::hardware_runtime
