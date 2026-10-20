#include "binding.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <toml++/toml.hpp>
#include <utility>

#include "class.h"
#include "src/eos/lib/log.h"

namespace eos::runtime::hardware_runtime {
namespace detail {

/// Throws the binding-level parse error: file name first, then the problem.
[[noreturn]] void Fail(const std::string& path, const std::string& what) {
  throw std::runtime_error("binding '" + path + "': " + what);
}

/// Feed class names in FeedDeclarations order, for messages and iteration.
/// The device classes: what a device is, independent of how EOS reaches it.
const char* const kClasses[] = {
    "arm", "base", "gimbal", "cam", "mic", "gripper", "speaker",
};

/// The backend types: how EOS reaches a device, independent of what it is.
const char* const kBackendTypes[] = {"demo", "ros2", "aplay", "alsa", "v4l2"};

const PrimitiveSpec kPrimitiveCatalog[] = {
    {"base.velocity", "1.0.0", "base", PrimitiveTier::kStandard, PrimitiveKind::kDirect, "",
     "move a base with body-frame velocity in m/s and rad/s"},
    {"base.navigate", "1.0.0", "base", PrimitiveTier::kStandard, PrimitiveKind::kDirect, "",
     "navigate a base to a relative or absolute planar goal"},
    {"base.posture", "1.0.0", "base", PrimitiveTier::kStandard, PrimitiveKind::kDirect, "",
     "set a base body posture"},
    {"gimbal.motion", "1.0.0", "gimbal", PrimitiveTier::kStandard, PrimitiveKind::kDirect, "",
     "point a gimbal with pan and tilt angles in radians"},
    {"gripper.motion", "1.0.0", "gripper", PrimitiveTier::kStandard, PrimitiveKind::kDirect, "",
     "open or close a gripper with bounded opening, speed and force"},
    {"speaker.play", "1.0.0", "speaker", PrimitiveTier::kStandard, PrimitiveKind::kDirect, "",
     "play one EOS-owned audio asset"},
    {"speaker.speak", "1.0.0", "speaker", PrimitiveTier::kStandard, PrimitiveKind::kAdvanced,
     "audio.synthesis", "synthesize text through the configured TTS dependency and play it"},
    {"face.register", "1.0.0", "cam", PrimitiveTier::kStandard, PrimitiveKind::kAdvanced,
     "vision.face", "register the face in the camera's latest frame"},
    {"arm.init", "1.0.0", "arm", PrimitiveTier::kOperator, PrimitiveKind::kDirect, "",
     "move an arm to its configured initial joint pose"},
    {"arm.movej", "1.0.0", "arm", PrimitiveTier::kOperator, PrimitiveKind::kDirect, "",
     "move every arm joint to target angles in radians"},
    {"arm.movel", "1.0.0", "arm", PrimitiveTier::kOperator, PrimitiveKind::kDirect, "",
     "move an arm end effector to a Cartesian pose"},
    {"arm.pick", "1.0.0", "arm", PrimitiveTier::kStandard, PrimitiveKind::kDirect, "",
     "pick one object selected by its typed world identifier"},
    {"motion.stop", "1.0.0", "motion", PrimitiveTier::kPrivileged, PrimitiveKind::kDirect, "",
     "clamp one motion device"},
    {"robot.stop", "1.0.0", "robot", PrimitiveTier::kPrivileged, PrimitiveKind::kDirect, "",
     "bring every motion-capable robot part to a controlled stop"},
    {"safety.estop", "1.0.0", "runtime", PrimitiveTier::kPrivileged, PrimitiveKind::kDirect, "",
     "halt the admitted runtime"},
};

/// One endpoint group of `[device.backend.ros2]`: the standard message type
/// EOS speaks on it. Vendor protocols never appear here — the deployment's
/// adapter converts at the graph edge.
struct EndpointSpec {
  const char* group;
  const char* message;  ///< the single standard type this group carries
};

const EndpointSpec kEndpointGroups[] = {
    {"joint_states", "sensor_msgs/JointState"},
    {"pose_query", "geometry_msgs/PoseStamped"},
    {"arm_status", "std_msgs/String"},
    {"stop", "std_msgs/Empty"},
    {"estop", "std_msgs/Empty"},
    {"movej", "std_msgs/Float64MultiArray"},
    {"movel", "std_msgs/Float64MultiArray"},
    {"pick", "std_msgs/String"},
    {"command_result", "std_msgs/String"},
    {"command_cancel", "std_msgs/String"},
    {"gripper_command", "std_msgs/Float64MultiArray"},
    {"gripper_state", "std_msgs/Float64MultiArray"},
    {"image", "sensor_msgs/CompressedImage"},
    {"depth_image", "sensor_msgs/Image"},
    {"health", "std_msgs/Bool"},
};

const EndpointSpec* EndpointSpecOf(const std::string& group) {
  for (const EndpointSpec& spec : kEndpointGroups) {
    if (group == spec.group) {
      return &spec;
    }
  }
  return nullptr;
}

const std::vector<std::string_view>& RequiredLimits(std::string_view primitive) {
  static const std::vector<std::string_view> kNone;
  static const std::vector<std::string_view> kBaseVelocity{"linear_x_m_s", "linear_y_m_s",
                                                           "angular_z_rad_s", "valid_for_ms"};
  static const std::vector<std::string_view> kBaseNavigateRelative{"x_m", "y_m", "yaw_rad"};
  static const std::vector<std::string_view> kGimbalPosition{"pan_rad", "tilt_rad"};
  static const std::vector<std::string_view> kArmMoveJ{"joint_positions_rad"};
  if (primitive == "base.velocity") return kBaseVelocity;
  if (primitive == "base.navigate") return kBaseNavigateRelative;
  if (primitive == "gimbal.motion") return kGimbalPosition;
  if (primitive == "arm.movej") return kArmMoveJ;
  return kNone;
}

const std::vector<std::string_view>& AllowedLimits(std::string_view primitive) {
  static const std::vector<std::string_view> kNone;
  static const std::vector<std::string_view> kBaseVelocity{"linear_x_m_s", "linear_y_m_s",
                                                           "angular_z_rad_s", "valid_for_ms"};
  static const std::vector<std::string_view> kBaseNavigateAllowed{"x_m", "y_m", "yaw_rad", "z_m", "speed"};
  static const std::vector<std::string_view> kGimbalPosition{"pan_rad", "tilt_rad", "speed_rad_s"};
  static const std::vector<std::string_view> kArmMoveJ{"joint_positions_rad", "speed_rad_s"};
  static const std::vector<std::string_view> kArmMoveL{"speed_m_s"};
  static const std::vector<std::string_view> kGripperCommand{"force_n", "opening_pct", "speed_pct"};
  if (primitive == "base.velocity") return kBaseVelocity;
  if (primitive == "base.navigate") return kBaseNavigateAllowed;
  if (primitive == "gimbal.motion") return kGimbalPosition;
  if (primitive == "arm.movej") return kArmMoveJ;
  if (primitive == "arm.movel") return kArmMoveL;
  if (primitive == "gripper.motion") return kGripperCommand;
  return kNone;
}

/// Reads a required string key from `tbl`. Fails loudly when missing or not
/// a string; `where` names the table in the message.
std::string RequireString(const toml::table& tbl, const std::string& key, const std::string& path,
                          const std::string& where) {
  const auto* node = tbl.get(key);
  if (node == nullptr) {
    Fail(path, where + ": missing required key '" + key + "'");
  }
  if (auto value = node->value<std::string>()) {
    return std::move(*value);
  }
  Fail(path, where + ": key '" + key + "' must be a string");
}

/// Reads an optional string key from `tbl`; nullopt when absent.
std::optional<std::string> OptionalString(const toml::table& tbl, const std::string& key,
                                          const std::string& path, const std::string& where) {
  const auto* node = tbl.get(key);
  if (node == nullptr) {
    return std::nullopt;
  }
  if (auto value = node->value<std::string>()) {
    return std::move(*value);
  }
  Fail(path, where + ": key '" + key + "' must be a string");
}

/// Reads a required non-negative integer key from `tbl` as uint64.
std::uint64_t RequireBytes(const toml::table& tbl, const std::string& key, const std::string& path,
                           const std::string& where) {
  const auto* node = tbl.get(key);
  if (node == nullptr) {
    Fail(path, where + ": missing required key '" + key + "'");
  }
  if (auto value = node->value<std::int64_t>()) {
    if (*value >= 0) {
      return static_cast<std::uint64_t>(*value);
    }
    Fail(path, where + ": key '" + key + "' must not be negative");
  }
  Fail(path, where + ": key '" + key + "' must be an integer");
}

/// Reads a required non-negative number key from `tbl` as double.
double RequireDouble(const toml::table& tbl, const std::string& key, const std::string& path,
                     const std::string& where) {
  const auto* node = tbl.get(key);
  if (node == nullptr) {
    Fail(path, where + ": missing required key '" + key + "'");
  }
  if (auto value = node->value<double>()) {
    if (*value >= 0.0) {
      return *value;
    }
    Fail(path, where + ": key '" + key + "' must not be negative");
  }
  Fail(path, where + ": key '" + key + "' must be a number");
}

/// Reads a required (possibly negative) coordinate key from `tbl` as double;
/// world-frame positions are signed, unlike rates and sizes.
double RequireCoordinate(const toml::table& tbl, const std::string& key, const std::string& path,
                         const std::string& where) {
  const auto* node = tbl.get(key);
  if (node == nullptr) {
    Fail(path, where + ": missing required key '" + key + "'");
  }
  if (auto value = node->value<double>()) {
    return *value;
  }
  Fail(path, where + ": key '" + key + "' must be a number");
}

/// Reads an optional non-negative scalar key from `tbl`, returning `fallback`
/// when absent; fails loudly on a non-number or negative value.
double OptionalLength(const toml::table& tbl, const std::string& key, double fallback,
                      const std::string& path, const std::string& where) {
  const auto* node = tbl.get(key);
  if (node == nullptr) {
    return fallback;
  }
  if (auto value = node->value<double>()) {
    if (*value >= 0.0) {
      return *value;
    }
    Fail(path, where + ": key '" + key + "' must not be negative");
  }
  Fail(path, where + ": key '" + key + "' must be a number");
}

/// Reads an optional signed coordinate key from `tbl`, returning `fallback`
/// when absent; fails loudly on a non-number value.
double OptionalCoordinate(const toml::table& tbl, const std::string& key, double fallback,
                          const std::string& path, const std::string& where) {
  const auto* node = tbl.get(key);
  if (node == nullptr) {
    return fallback;
  }
  if (auto value = node->value<double>()) {
    return *value;
  }
  Fail(path, where + ": key '" + key + "' must be a number");
}

/// Reads an optional non-negative integer key from `tbl` as uint64.
std::uint64_t OptionalBytes(const toml::table& tbl, const std::string& key, std::uint64_t fallback,
                            const std::string& path, const std::string& where) {
  const auto* node = tbl.get(key);
  if (node == nullptr) {
    return fallback;
  }
  if (auto value = node->value<std::int64_t>()) {
    if (*value >= 0) {
      return static_cast<std::uint64_t>(*value);
    }
    Fail(path, where + ": key '" + key + "' must not be negative");
  }
  Fail(path, where + ": key '" + key + "' must be an integer");
}

/// Fails unless every key of `tbl` is in the NUL-terminated `allowed` list.
void RequireKnownKeys(const toml::table& tbl, const char* const* allowed, const std::string& path,
                      const std::string& where) {
  for (const auto& [key, value] : tbl) {
    (void)value;
    bool known = false;
    for (const char* const* candidate = allowed; *candidate != nullptr; ++candidate) {
      if (key == *candidate) {
        known = true;
        break;
      }
    }
    if (!known) {
      Fail(path, where + ": unknown key '" + std::string(key.str()) + "'");
    }
  }
}

/// Parses one `[device.feeds.<class>]` table into a rate limit.
hardware::FeedEnvelope ParseEnvelope(const toml::table& tbl, const std::string& feed_class,
                                     const std::string& path, const std::string& where) {
  const std::string scope = where + " feed '" + feed_class + "'";
  static const char* const kAllowed[] = {"max_rate_hz", nullptr};
  RequireKnownKeys(tbl, kAllowed, path, scope);
  return hardware::FeedEnvelope{
      .max_rate_hz = RequireDouble(tbl, "max_rate_hz", path, scope),
  };
}

/// Parses a `[device.feeds]` table. Health is required; omitted data feed
/// classes default to disabled (0 Hz), keeping bindings concise.
hardware::FeedDeclarations ParseFeeds(const toml::table& feeds, const std::string& path,
                                      const std::string& where) {
  hardware::FeedDeclarations parsed{};
  bool health_seen = false;
  for (const auto& [key, value] : feeds) {
    const std::string name(key.str());
    const auto* table = value.as_table();
    if (table == nullptr) {
      Fail(path, where + " feed '" + name + "': must be a table");
    }
    if (name == "geometric") {
      parsed.geometric = ParseEnvelope(*table, name, path, where);
    } else if (name == "proprioception") {
      parsed.proprioception = ParseEnvelope(*table, name, path, where);
    } else if (name == "sensor") {
      parsed.sensor = ParseEnvelope(*table, name, path, where);
    } else if (name == "health") {
      parsed.health = ParseEnvelope(*table, name, path, where);
      health_seen = true;
    } else {
      Fail(path, where + ": unknown feed class '" + name + "'");
    }
  }
  if (!health_seen) {
    Fail(path, where + ": missing feed class 'health'");
  }
  return parsed;
}

void RequireNoPartKeys(const toml::table& table, const PartConfig& config, const std::string& path,
                       const std::string& where) {
  if (table.empty()) return;
  const std::string key(table.begin()->first.str());
  Fail(path, where + ": unknown key '" + key + "' for kind '" + config.kind + "'");
}

/// Reads an optional 1..6 string-array key from `tbl` into joint names.
/// Returns empty (meaning "use joint_1..joint_6") when the key is absent.
std::vector<std::string> OptionalJointNames(const toml::table& tbl, const std::string& key,
                                            const std::string& path, const std::string& where) {
  std::vector<std::string> names;
  const auto* node = tbl.get(key);
  if (node == nullptr) return names;
  const auto* array = node->as_array();
  if (array == nullptr) {
    Fail(path, where + ": key '" + key + "' must be an array of strings");
  }
  for (const auto& element : *array) {
    if (auto value = element.value<std::string>()) {
      if (value->empty()) {
        Fail(path, where + ": key '" + key + "' must not contain empty joint names");
      }
      if (std::find(names.begin(), names.end(), *value) != names.end()) {
        Fail(path, where + ": key '" + key + "' must contain distinct joint names");
      }
      names.push_back(std::move(*value));
    } else {
      Fail(path, where + ": key '" + key + "' must be an array of strings");
    }
  }
  if (names.empty() || names.size() > 6) {
    Fail(path, where + ": key '" + key + "' must hold 1..6 joint names");
  }
  return names;
}

/// Populates the class's typed config with its documented defaults so the
/// member is present even when the binding omits [device.part] entirely.
void ApplyPartDefaults(PartConfig& part) {
  if (part.kind == "base") {
    ApplyBaseDefaults(part);
  } else if (part.kind == "gimbal") {
    ApplyGimbalDefaults(part);
  } else if (part.kind == "cam") {
    ApplyCameraDefaults(part);
  } else if (part.kind == "arm") {
    ApplyArmDefaults(part);
  }
}

/// Parses one `[device.part]` table into the class's typed config. The
/// class's member already carries documented defaults from ApplyPartDefaults;
/// only present keys are read (typed, validated). Any key the class does not
/// define fails loudly as a typo.
void ParsePartTable(const toml::table& part, PartConfig& cfg, const std::string& path,
                    const std::string& where) {
  if (cfg.kind == "base") {
    ParseBasePart(part, cfg, path, where);
  } else if (cfg.kind == "gimbal") {
    ParseGimbalPart(part, cfg, path, where);
  } else if (cfg.kind == "cam") {
    ParseCameraPart(part, cfg, path, where);
  } else if (cfg.kind == "arm") {
    ParseArmPart(part, cfg, path, where);
  } else if (cfg.kind == "gripper") {
    ParseGripperPart(part, cfg, path, where);
  } else if (cfg.kind == "mic") {
    ParseMicPart(part, cfg, path, where);
  } else if (cfg.kind == "speaker") {
    ParseSpeakerPart(part, cfg, path, where);
  }
}

/// Parses `[device.backend.ros2]`: scalars plus strict-keyed endpoint groups.
Ros2DeviceConfig ParseRos2Table(const toml::table& tbl, const std::string& path,
                                const std::string& where) {
  static const char* const kAllowed[] = {"domain_id",
                                         "namespace",
                                         "base_frame",
                                         "telemetry_timeout_ms",
                                         "command_timeout_ms",
                                         "pick_timeout_ms",
                                         "joint_states",
                                         "pose_query",
                                         "arm_status",
                                         "stop",
                                         "estop",
                                         "movej",
                                         "movel",
                                         "pick",
                                         "command_result",
                                         "command_cancel",
                                         "gripper_command",
                                         "gripper_state",
                                         "image",
                                         "depth_image",
                                         "depth_scale",
                                         "health",
                                         "gimbal_joint_map",
                                         "max_image_bytes",
                                         nullptr};
  RequireKnownKeys(tbl, kAllowed, path, where);
  Ros2DeviceConfig config;
  config.domain_id = RequireBytes(tbl, "domain_id", path, where);
  if (config.domain_id > 232) {
    Fail(path, where + ": key 'domain_id' must be between 0 and 232");
  }
  if (const auto ns = OptionalString(tbl, "namespace", path, where); ns.has_value()) {
    config.node_namespace = std::move(*ns);
  } else {
    Fail(path, where + ": missing required key 'namespace'");
  }
  if (const auto frame = OptionalString(tbl, "base_frame", path, where); frame.has_value()) {
    config.base_frame = std::move(*frame);
  }
  config.telemetry_timeout_ms =
      OptionalBytes(tbl, "telemetry_timeout_ms", config.telemetry_timeout_ms, path, where);
  config.command_timeout_ms =
      OptionalBytes(tbl, "command_timeout_ms", config.command_timeout_ms, path, where);
  config.pick_timeout_ms =
      OptionalBytes(tbl, "pick_timeout_ms", config.pick_timeout_ms, path, where);
  config.max_image_bytes =
      OptionalBytes(tbl, "max_image_bytes", config.max_image_bytes, path, where);
  config.depth_scale = OptionalLength(tbl, "depth_scale", config.depth_scale, path, where);
  if (config.max_image_bytes == 0 || config.max_image_bytes > 16 * 1024 * 1024) {
    Fail(path, where + ": key 'max_image_bytes' must be within 1..16777216");
  }

  if (config.telemetry_timeout_ms == 0 || config.telemetry_timeout_ms > 60000) {
    Fail(path, where + ": telemetry_timeout_ms must be within 1..60000");
  }
  if (config.command_timeout_ms == 0 || config.command_timeout_ms > 300000) {
    Fail(path, where + ": command_timeout_ms must be within 1..300000");
  }
  if ((tbl.get("pick_timeout_ms") != nullptr && config.pick_timeout_ms == 0) ||
      config.pick_timeout_ms > 300000) {
    Fail(path, where + ": pick_timeout_ms must be within 1..300000 when set");
  }
  if (!std::isfinite(config.depth_scale) || config.depth_scale <= 0.0) {
    Fail(path, where + ": key 'depth_scale' must be positive and finite");
  }
  if (const auto* mapping = tbl.get("gimbal_joint_map"); mapping != nullptr) {
    const auto* table = mapping->as_table();
    if (table == nullptr) Fail(path, where + ": key 'gimbal_joint_map' must be a table");
    static const char* const kMappingKeys[] = {"arm_part_id", "pan_joint", "tilt_joint",
                                               "tilt_direction", nullptr};
    RequireKnownKeys(*table, kMappingKeys, path, where + " [gimbal_joint_map]");
    Ros2GimbalJointMap parsed;
    parsed.arm_part_id = RequireString(*table, "arm_part_id", path, where + " [gimbal_joint_map]");
    parsed.pan_joint = RequireString(*table, "pan_joint", path, where + " [gimbal_joint_map]");
    parsed.tilt_joint = RequireString(*table, "tilt_joint", path, where + " [gimbal_joint_map]");
    if (const auto direction = table->get("tilt_direction"); direction != nullptr) {
      const auto value = direction->value<std::int64_t>();
      if (!value || (*value != -1 && *value != 1))
        Fail(path, where + " [gimbal_joint_map]: tilt_direction must be -1 or 1");
      parsed.tilt_direction = static_cast<int>(*value);
    }
    if (parsed.pan_joint == parsed.tilt_joint) {
      Fail(path, where + ": gimbal pan_joint and tilt_joint must be distinct");
    }
    config.gimbal_joint_map = std::move(parsed);
  }
  auto endpoint = [&](const std::string& group) -> Ros2Endpoint& {
    if (group == "joint_states") return config.joint_states;
    if (group == "pose_query") return config.pose_query;
    if (group == "arm_status") return config.arm_status;
    if (group == "stop") return config.stop;
    if (group == "estop") return config.estop;
    if (group == "movej") return config.movej;
    if (group == "movel") return config.movel;
    if (group == "pick") return config.pick;
    if (group == "command_result") return config.command_result;
    if (group == "command_cancel") return config.command_cancel;
    if (group == "gripper_command") return config.gripper_command;
    if (group == "gripper_state") return config.gripper_state;
    if (group == "depth_image") return config.depth_image;
    if (group == "health") return config.health;
    return config.image;
  };
  for (const EndpointSpec& spec : kEndpointGroups) {
    const auto* node = tbl.get(spec.group);
    if (node == nullptr) {
      continue;
    }
    const auto* table = node->as_table();
    if (table == nullptr) {
      Fail(path, where + ": key '" + std::string(spec.group) + "' must be a table");
    }
    const std::string scope = where + " endpoint '" + std::string(spec.group) + "'";
    static const char* const kGroupKeys[] = {"kind", "name", "message", nullptr};
    RequireKnownKeys(*table, kGroupKeys, path, scope);
    Ros2Endpoint parsed;
    parsed.kind = RequireString(*table, "kind", path, scope);
    if (parsed.kind != "topic") {
      // Parsed but not executable: only streaming topics are implemented.
      // A goal-shaped endpoint (action/service) is a rejected admission.
      Fail(path, scope + ": kind '" + parsed.kind +
                     "' is not implemented yet; "
                     "only 'topic' endpoints can be admitted");
    }
    parsed.name = RequireString(*table, "name", path, scope);
    parsed.message = RequireString(*table, "message", path, scope);
    if (parsed.message != spec.message) {
      Fail(path, scope + ": message must be '" + std::string(spec.message) +
                     "' (standard common interfaces only; vendor protocols live in the "
                     "deployment adapter)");
    }
    endpoint(spec.group) = std::move(parsed);
  }
  return config;
}

AplayDeviceConfig ParseAplayTable(const toml::table& table, const std::string& path,
                                  const std::string& where) {
  static const char* const kAllowed[] = {"alsa_device", "max_stream_bytes", nullptr};
  RequireKnownKeys(table, kAllowed, path, where);
  AplayDeviceConfig config;
  if (const auto device = OptionalString(table, "alsa_device", path, where); device.has_value()) {
    config.alsa_device = *device;
  }
  if (config.alsa_device.empty()) Fail(path, where + ": key 'alsa_device' must not be empty");
  config.max_stream_bytes =
      OptionalBytes(table, "max_stream_bytes", config.max_stream_bytes, path, where);
  if (config.max_stream_bytes == 0) {
    Fail(path, where + ": key 'max_stream_bytes' must be positive");
  }
  return config;
}

AlsaMicDeviceConfig ParseAlsaMicTable(const toml::table& table, const std::string& path,
                                      const std::string& where) {
  static const char* const kAllowed[] = {"alsa_device", "frame_id",      "sample_rate_hz",
                                         "channels",    "window_frames", "read_timeout_ms",
                                         nullptr};
  RequireKnownKeys(table, kAllowed, path, where);
  AlsaMicDeviceConfig config;
  if (const auto device = OptionalString(table, "alsa_device", path, where); device.has_value()) {
    config.alsa_device = *device;
  }
  if (config.alsa_device.empty()) Fail(path, where + ": key 'alsa_device' must not be empty");
  if (const auto frame = OptionalString(table, "frame_id", path, where); frame.has_value()) {
    config.frame_id = *frame;
  }
  if (config.frame_id.empty()) Fail(path, where + ": key 'frame_id' must not be empty");
  const std::uint64_t rate =
      OptionalBytes(table, "sample_rate_hz", config.sample_rate_hz, path, where);
  if (rate == 0 || rate > 192000) {
    Fail(path, where + ": key 'sample_rate_hz' must be within 1..192000");
  }
  config.sample_rate_hz = static_cast<std::uint32_t>(rate);
  const std::uint64_t channels = OptionalBytes(table, "channels", config.channels, path, where);
  if (channels == 0 || channels > 8) {
    Fail(path, where + ": key 'channels' must be within 1..8");
  }
  config.channels = static_cast<std::uint16_t>(channels);
  const std::uint64_t window_frames =
      OptionalBytes(table, "window_frames", config.window_frames, path, where);
  if (window_frames == 0 || window_frames > 48000) {
    Fail(path, where + ": key 'window_frames' must be within 1..48000");
  }
  config.window_frames = static_cast<std::uint32_t>(window_frames);
  const std::uint64_t read_timeout_ms =
      OptionalBytes(table, "read_timeout_ms", config.read_timeout_ms, path, where);
  if (read_timeout_ms == 0 || read_timeout_ms > 60000) {
    Fail(path, where + ": key 'read_timeout_ms' must be within 1..60000");
  }
  config.read_timeout_ms = static_cast<std::uint32_t>(read_timeout_ms);
  return config;
}

V4l2CamDeviceConfig ParseV4l2CamTable(const toml::table& table, const std::string& path,
                                      const std::string& where) {
  static const char* const kAllowed[] = {"video_device",    "frame_id", "width_px",
                                         "height_px",       "fps",      "read_timeout_ms",
                                         "max_frame_bytes", nullptr};
  RequireKnownKeys(table, kAllowed, path, where);
  V4l2CamDeviceConfig config;
  if (const auto device = OptionalString(table, "video_device", path, where); device.has_value()) {
    config.video_device = *device;
  }
  if (config.video_device.empty()) Fail(path, where + ": key 'video_device' must not be empty");
  if (const auto frame = OptionalString(table, "frame_id", path, where); frame.has_value()) {
    config.frame_id = *frame;
  }
  if (config.frame_id.empty()) Fail(path, where + ": key 'frame_id' must not be empty");
  const std::uint64_t width = OptionalBytes(table, "width_px", config.width_px, path, where);
  if (width == 0 || width > 16384) {
    Fail(path, where + ": key 'width_px' must be within 1..16384");
  }
  config.width_px = static_cast<std::uint32_t>(width);
  const std::uint64_t height = OptionalBytes(table, "height_px", config.height_px, path, where);
  if (height == 0 || height > 16384) {
    Fail(path, where + ": key 'height_px' must be within 1..16384");
  }
  config.height_px = static_cast<std::uint32_t>(height);
  const std::uint64_t fps = OptionalBytes(table, "fps", config.fps, path, where);
  if (fps == 0 || fps > 240) Fail(path, where + ": key 'fps' must be within 1..240");
  config.fps = static_cast<std::uint32_t>(fps);
  const std::uint64_t read_timeout_ms =
      OptionalBytes(table, "read_timeout_ms", config.read_timeout_ms, path, where);
  if (read_timeout_ms == 0 || read_timeout_ms > 60000) {
    Fail(path, where + ": key 'read_timeout_ms' must be within 1..60000");
  }
  config.read_timeout_ms = static_cast<std::uint32_t>(read_timeout_ms);
  config.max_frame_bytes =
      OptionalBytes(table, "max_frame_bytes", config.max_frame_bytes, path, where);
  if (config.max_frame_bytes == 0 || config.max_frame_bytes > 64 * 1024 * 1024) {
    Fail(path, where + ": key 'max_frame_bytes' must be within 1..67108864");
  }
  return config;
}

NumericRange ParseRange(const toml::table& table, const std::string& path,
                        const std::string& where) {
  static const char* const kAllowed[] = {"minimum", "maximum", "minimum_exclusive",
                                         "maximum_exclusive", nullptr};
  RequireKnownKeys(table, kAllowed, path, where);
  NumericRange range;
  if (table.get("minimum") != nullptr) {
    range.minimum = RequireCoordinate(table, "minimum", path, where);
  }
  if (table.get("maximum") != nullptr) {
    range.maximum = RequireCoordinate(table, "maximum", path, where);
  }
  if (const auto* value = table.get("minimum_exclusive"); value != nullptr) {
    const auto parsed = value->value<bool>();
    if (!parsed.has_value()) Fail(path, where + ": key 'minimum_exclusive' must be a boolean");
    range.minimum_exclusive = *parsed;
  }
  if (const auto* value = table.get("maximum_exclusive"); value != nullptr) {
    const auto parsed = value->value<bool>();
    if (!parsed.has_value()) Fail(path, where + ": key 'maximum_exclusive' must be a boolean");
    range.maximum_exclusive = *parsed;
  }
  const bool invalid_order =
      range.minimum.has_value() && range.maximum.has_value() &&
      (*range.minimum > *range.maximum ||
       (*range.minimum == *range.maximum && (range.minimum_exclusive || range.maximum_exclusive)));
  if ((!range.minimum.has_value() && !range.maximum.has_value()) ||
      (range.minimum_exclusive && !range.minimum.has_value()) ||
      (range.maximum_exclusive && !range.maximum.has_value()) ||
      (range.minimum.has_value() && !std::isfinite(*range.minimum)) ||
      (range.maximum.has_value() && !std::isfinite(*range.maximum)) || invalid_order) {
    Fail(path, where + ": bounds do not form a non-empty finite range");
  }
  return range;
}

void ParseLimits(const toml::table& primitive_table, PrimitiveDeclaration& primitive,
                 const std::string& path, const std::string& where) {
  const std::vector<std::string_view>& required = RequiredLimits(primitive.name);
  const std::vector<std::string_view>& allowed = AllowedLimits(primitive.name);
  const auto* node = primitive_table.get("limits");
  if (allowed.empty()) {
    if (node != nullptr) Fail(path, where + ": primitive has no device-specific numeric limits");
    return;
  }
  if (node == nullptr) {
    if (required.empty()) return;
    Fail(path, where + ": key 'limits' must be a table containing every required range");
  }
  if (node->as_table() == nullptr) {
    Fail(path, where + ": key 'limits' must be a table containing numeric ranges");
  }
  const toml::table& limits = *node->as_table();
  for (const auto& [key, value] : limits) {
    const std::string name(key.str());
    if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
      Fail(path, where + ": unknown limit '" + name + "' for primitive '" + primitive.name + "'");
    }
    const auto* range = value.as_table();
    if (range == nullptr) Fail(path, where + ": limit '" + name + "' must be a table");
    primitive.limits.emplace(name, ParseRange(*range, path, where + " limit '" + name + "'"));
  }
  for (const std::string_view name : required) {
    if (primitive.limits.find(std::string(name)) == primitive.limits.end()) {
      Fail(path, where + ": missing required limit '" + std::string(name) + "'");
    }
  }
  for (const std::string_view name : allowed) {
    const bool is_required = std::find(required.begin(), required.end(), name) != required.end();
    if (!is_required && primitive.limits.find(std::string(name)) == primitive.limits.end()) {
      lib::LogEvent(
          lib::GetLogger("hardware"), spdlog::level::info, "primitive_limit_uses_backend_default",
          {{"binding", path}, {"primitive", primitive.name}, {"parameter", std::string(name)}});
    }
  }
}

/// Parses the `[[device.primitive]]` declarations and admits each against the
/// primitive catalog: the primitive must exist, belong to the device's class, and be
/// executable by the device's backend type.
void ParsePrimitives(const toml::array& array, DeviceBinding& device, const std::string& path,
                     const std::string& where) {
  int index = 0;
  for (const auto& entry : array) {
    const auto* table = entry.as_table();
    if (table == nullptr) {
      Fail(path, where + ": each primitive must be a table");
    }
    const std::string scope = where + " primitive[" + std::to_string(index) + "]";
    static const char* const kAllowed[] = {"name",           "version", "description",
                                           "model_id",       "tier",    "references",
                                           "absolute_frame", "limits",  nullptr};
    RequireKnownKeys(*table, kAllowed, path, scope);
    PrimitiveDeclaration primitive;
    primitive.name = RequireString(*table, "name", path, scope);
    primitive.version = RequireString(*table, "version", path, scope);
    primitive.description = RequireString(*table, "description", path, scope);
    const PrimitiveSpec* spec = PrimitiveSpecOf(primitive.name, primitive.version);
    if (spec == nullptr) {
      Fail(path, scope + ": unknown primitive '" + primitive.name + "' version '" +
                     primitive.version + "'");
    }
    if (spec->tier == PrimitiveTier::kPrivileged) {
      Fail(path, scope + ": privileged primitive '" + primitive.name +
                     "' is implicit and cannot be declared");
    }
    // The catalog fixes the tier; an omitted `tier` adopts it, an explicit
    // one must agree.
    primitive.tier = spec->tier;
    if (const auto tier = OptionalString(*table, "tier", path, scope); tier.has_value()) {
      if (*tier == "standard") {
        primitive.tier = PrimitiveTier::kStandard;
      } else if (*tier == "operator") {
        primitive.tier = PrimitiveTier::kOperator;
      } else {
        Fail(path, scope + ": key 'tier' must be standard or operator");
      }
    }
    if (primitive.tier != spec->tier) {
      Fail(path, scope + ": primitive '" + primitive.name + "' is declared '" +
                     (primitive.tier == PrimitiveTier::kOperator ? "operator" : "standard") +
                     "' but the catalog fixes the other tier");
    }
    if (device.part.kind != spec->device_class) {
      Fail(path, scope + ": primitive '" + primitive.name + "' belongs to class '" +
                     spec->device_class + "', device class is '" + device.part.kind + "'");
    }
    if (!BackendCanExecutePrimitive(*spec, device.backend_type)) {
      Fail(path, scope + ": backend type '" + device.backend_type + "' cannot execute primitive '" +
                     primitive.name + "'");
    }
    const std::optional<std::string> model_id = OptionalString(*table, "model_id", path, scope);
    primitive.model_id = model_id.value_or("");
    if (spec->kind == PrimitiveKind::kAdvanced && primitive.model_id.empty()) {
      Fail(path,
           scope + ": advanced primitive '" + primitive.name + "' requires a non-empty model_id");
    }
    if (spec->kind == PrimitiveKind::kDirect && model_id.has_value()) {
      Fail(path, scope + ": key 'model_id' is only valid for advanced primitives");
    }
    const bool referenced_motion = primitive.name == "base.navigate" ||
                                   primitive.name == "gimbal.motion" ||
                                   primitive.name == "arm.movej" || primitive.name == "arm.movel";
    if (const auto* references = table->get("references"); references != nullptr) {
      if (!referenced_motion) {
        Fail(path, scope + ": key 'references' is only valid for reference-aware motion");
      }
      const auto* values = references->as_array();
      if (values == nullptr || values->empty()) {
        Fail(path, scope + ": key 'references' must be a non-empty array");
      }
      for (const auto& entry : *values) {
        const auto value = entry.value<std::string>();
        if (!value.has_value() || (*value != "relative" && *value != "absolute")) {
          Fail(path, scope + ": key 'references' may contain only relative or absolute");
        }
        if (std::find(primitive.references.begin(), primitive.references.end(), *value) !=
            primitive.references.end()) {
          Fail(path, scope + ": key 'references' contains duplicate value '" + *value + "'");
        }
        primitive.references.push_back(*value);
      }
    } else if (referenced_motion) {
      Fail(path, scope + ": reference-aware primitive '" + primitive.name +
                     "' requires key 'references'");
    }
    primitive.absolute_frame = OptionalString(*table, "absolute_frame", path, scope);
    if (primitive.absolute_frame.has_value() && primitive.absolute_frame->empty()) {
      Fail(path, scope + ": key 'absolute_frame' must not be empty");
    }
    const bool admits_absolute = std::find(primitive.references.begin(), primitive.references.end(),
                                           "absolute") != primitive.references.end();
    const bool spatial_target = primitive.name == "base.navigate" ||
                                primitive.name == "gimbal.motion" || primitive.name == "arm.movel";
    if (admits_absolute && spatial_target && !primitive.absolute_frame.has_value()) {
      Fail(path, scope + ": absolute reference requires key 'absolute_frame'");
    }
    if (primitive.absolute_frame.has_value() && (!spatial_target || !admits_absolute)) {
      Fail(path, scope + ": key 'absolute_frame' requires an absolute spatial-motion reference");
    }
    ParseLimits(*table, primitive, path, scope);
    device.part.primitives.push_back(std::move(primitive));
    ++index;
  }
}

/// Fails when a declared command needs a ros2 endpoint group the device's
/// transport table does not carry, or when the always-required groups are
/// missing.
void CheckRos2Endpoints(const DeviceBinding& device, const std::string& path,
                        const std::string& where) {
  const Ros2DeviceConfig& ros2 = *device.part.ros2;
  auto require = [&](const Ros2Endpoint& group, const char* name, const std::string& why) {
    if (group.name.empty()) {
      Fail(path, where + ": endpoint '" + std::string(name) + "' is required (" + why +
                     ") but the [device.backend.ros2] table omits it");
    }
  };
  if (device.part.kind == "arm") {
    require(ros2.joint_states, "joint_states", "joint telemetry");
    require(ros2.pose_query, "pose_query", "end-effector pose telemetry");
    require(ros2.stop, "stop", "motion.stop");
    require(ros2.estop, "estop", "safety.estop");
  } else if (device.part.kind == "gimbal") {
    require(ros2.joint_states, "joint_states", "joint-backed gimbal telemetry");
    require(ros2.movej, "movej", "joint-backed gimbal command");
    require(ros2.stop, "stop", "motion.stop");
    require(ros2.estop, "estop", "safety.estop");
    require(ros2.health, "health", "shared arm adapter health");
  } else if (device.part.kind == "gripper") {
    require(ros2.gripper_state, "gripper_state", "gripper telemetry");
    require(ros2.estop, "estop", "safety.estop");
  } else if (device.part.kind == "cam") {
    require(ros2.image, "image", "camera sensor feed");
  }
  for (const PrimitiveDeclaration& primitive : device.part.primitives) {
    if (primitive.name == "arm.init" || primitive.name == "arm.movej") {
      require(ros2.movej, "movej", "primitive " + primitive.name);
    } else if (primitive.name == "arm.movel") {
      require(ros2.movel, "movel", "primitive arm.movel");
    } else if (primitive.name == "arm.pick") {
      require(ros2.pick, "pick", "primitive arm.pick");
    } else if (primitive.name == "gripper.motion") {
      require(ros2.gripper_command, "gripper_command", "primitive gripper.motion");
    }
    const bool cancellable = primitive.name == "base.velocity" ||
                             primitive.name == "base.navigate" ||
                             primitive.name == "base.posture" ||
                             primitive.name == "gimbal.motion" || primitive.name == "arm.init" ||
                             primitive.name == "arm.movej" || primitive.name == "arm.movel" ||
                             primitive.name == "arm.pick" || primitive.name == "gripper.motion";
    if (cancellable) {
      require(ros2.command_result, "command_result", "cancellable primitive " + primitive.name);
      require(ros2.command_cancel, "command_cancel", "cancellable primitive " + primitive.name);
    }
  }
}

/// Fills the class-independent optional identity fields of a device table.
void ParseIdentity(const toml::table& tbl, PartConfig& part, const std::string& path,
                   const std::string& where) {
  if (const auto* description = tbl.get("description"); description != nullptr) {
    if (auto value = description->value<std::string>()) {
      part.description = std::move(*value);
    } else {
      Fail(path, where + ": key 'description' must be a string");
    }
  }
  if (const auto* tags = tbl.get("tags"); tags != nullptr) {
    const auto* array = tags->as_array();
    if (array == nullptr) {
      Fail(path, where + ": key 'tags' must be an array of strings");
    }
    for (const auto& tag : *array) {
      if (auto value = tag.value<std::string>()) {
        part.tags.push_back(std::move(*value));
      } else {
        Fail(path, where + ": key 'tags' must contain only strings");
      }
    }
  }
  if (const auto* part_id = tbl.get("part_id"); part_id != nullptr) {
    if (auto value = part_id->value<std::string>()) {
      part.part_id = std::move(*value);
    } else {
      Fail(path, where + ": key 'part_id' must be a string");
    }
  }
  if (const auto* ee = tbl.get("end_effector"); ee != nullptr) {
    if (auto value = ee->value<std::string>()) {
      part.end_effector = std::move(*value);
    } else {
      Fail(path, where + ": key 'end_effector' must be a string");
    }
  }
}

/// Parses one `[[device]]` entry.
DeviceBinding ParseDevice(const toml::table& tbl, const std::string& path,
                          const std::string& where) {
  static const char* const kAllowed[] = {"class",        "description", "tags",        "part_id",
                                         "end_effector", "part",        "joint_names", "primitive",
                                         "feeds",        "backend",     nullptr};
  if (tbl.contains("id")) {
    Fail(path, where + ": obsolete key 'id'; EOS generates device IDs as <class>-<index>");
  }
  RequireKnownKeys(tbl, kAllowed, path, where);
  DeviceBinding device;
  device.part.kind = RequireString(tbl, "class", path, where);
  bool known_class = false;
  for (const char* allowed : kClasses) {
    if (device.part.kind == allowed) {
      known_class = true;
      break;
    }
  }
  if (!known_class) {
    Fail(path, where + ": unknown class '" + device.part.kind + "'");
  }
  ApplyPartDefaults(device.part);
  ParseIdentity(tbl, device.part, path, where);

  if (const auto* part = tbl.get("part"); part != nullptr) {
    const auto* table = part->as_table();
    if (table == nullptr) {
      Fail(path, where + ": key 'part' must be a table");
    }
    ParsePartTable(*table, device.part, path, where + " [device.part]");
  }
  if (const auto* names = tbl.get("joint_names"); names != nullptr) {
    // Ordered URDF joint names are the ROS 2 command/state channel order.
    // Ranges, types, and velocity ceilings remain authoritative URDF data.
    if (device.part.kind != "arm" && device.part.kind != "gimbal") {
      Fail(path, where + ": key 'joint_names' is only valid for class 'arm' or 'gimbal'");
    }
    device.part.joint_names = OptionalJointNames(tbl, "joint_names", path, where);
  }
  if (const auto* backend = tbl.get("backend"); backend != nullptr) {
    const auto* table = backend->as_table();
    if (table == nullptr) {
      Fail(path, where + ": key 'backend' must be a table");
    }
    const std::string scope = where + " [device.backend]";
    static const char* const kAllowed[] = {"type", "ros2", "aplay", "alsa", "v4l2", nullptr};
    RequireKnownKeys(*table, kAllowed, path, scope);
    device.backend_type = RequireString(*table, "type", path, scope);
    bool known_type = false;
    for (const char* allowed : kBackendTypes) {
      if (device.backend_type == allowed) {
        known_type = true;
        break;
      }
    }
    if (!known_type) {
      Fail(path, scope + ": unknown backend type '" + device.backend_type + "'");
    }
    if (const auto* ros2 = table->get("ros2"); ros2 != nullptr) {
      if (device.backend_type != "ros2") {
        Fail(path, scope + ": key 'ros2' is only valid for backend type 'ros2'");
      }
      if (device.part.kind != "arm" && device.part.kind != "gimbal" &&
          device.part.kind != "gripper" && device.part.kind != "cam") {
        Fail(path, scope +
                       ": backend type 'ros2' is only implemented for class 'arm', 'gimbal', "
                       "'gripper', or 'cam'");
      }
      const auto* ros2_table = ros2->as_table();
      if (ros2_table == nullptr) {
        Fail(path, scope + ": key 'ros2' must be a table");
      }
      device.part.ros2 = ParseRos2Table(*ros2_table, path, scope + " [device.backend.ros2]");
    }
    if (const auto* aplay = table->get("aplay"); aplay != nullptr) {
      if (device.backend_type != "aplay") {
        Fail(path, scope + ": key 'aplay' is only valid for backend type 'aplay'");
      }
      if (device.part.kind != "speaker") {
        Fail(path, scope + ": backend type 'aplay' is only implemented for class 'speaker'");
      }
      const auto* aplay_table = aplay->as_table();
      if (aplay_table == nullptr) Fail(path, scope + ": key 'aplay' must be a table");
      device.part.aplay = ParseAplayTable(*aplay_table, path, scope + " [device.backend.aplay]");
    }
    if (const auto* alsa = table->get("alsa"); alsa != nullptr) {
      if (device.backend_type != "alsa") {
        Fail(path, scope + ": key 'alsa' is only valid for backend type 'alsa'");
      }
      if (device.part.kind != "mic") {
        Fail(path, scope + ": backend type 'alsa' is only implemented for class 'mic'");
      }
      const auto* alsa_table = alsa->as_table();
      if (alsa_table == nullptr) Fail(path, scope + ": key 'alsa' must be a table");
      device.part.alsa = ParseAlsaMicTable(*alsa_table, path, scope + " [device.backend.alsa]");
    }
    if (const auto* v4l2 = table->get("v4l2"); v4l2 != nullptr) {
      if (device.backend_type != "v4l2") {
        Fail(path, scope + ": key 'v4l2' is only valid for backend type 'v4l2'");
      }
      if (device.part.kind != "cam") {
        Fail(path, scope + ": backend type 'v4l2' is only implemented for class 'cam'");
      }
      const auto* v4l2_table = v4l2->as_table();
      if (v4l2_table == nullptr) Fail(path, scope + ": key 'v4l2' must be a table");
      device.part.v4l2 = ParseV4l2CamTable(*v4l2_table, path, scope + " [device.backend.v4l2]");
    }
  } else {
    Fail(path, where + ": missing required table 'backend'");
  }
  if (device.backend_type == "ros2" && !device.part.ros2.has_value()) {
    Fail(path, where + ": backend type 'ros2' requires a [device.backend.ros2] table");
  }
  if (device.backend_type == "ros2" && device.part.kind == "gimbal") {
    if (device.part.joint_names.size() != 6) {
      Fail(path, where + ": ROS 2 joint-backed gimbal requires exactly six joint_names");
    }
    if (!device.part.ros2->gimbal_joint_map.has_value()) {
      Fail(path, where + ": ROS 2 gimbal requires [device.backend.ros2.gimbal_joint_map]");
    }
    const auto& mapping = *device.part.ros2->gimbal_joint_map;
    for (const std::string* name : {&mapping.pan_joint, &mapping.tilt_joint}) {
      if (std::find(device.part.joint_names.begin(), device.part.joint_names.end(), *name) ==
          device.part.joint_names.end()) {
        Fail(path, where + ": mapped gimbal joint '" + *name + "' is not in joint_names");
      }
    }
  } else if (device.part.ros2.has_value() && device.part.ros2->gimbal_joint_map.has_value()) {
    Fail(path, where + ": gimbal_joint_map is only valid for a ROS 2 gimbal");
  }
  if (device.backend_type == "aplay" && !device.part.aplay.has_value()) {
    Fail(path, where + ": backend type 'aplay' requires a [device.backend.aplay] table");
  }
  if (device.backend_type == "alsa" && !device.part.alsa.has_value() && device.part.kind == "mic") {
    Fail(path, where + ": backend type 'alsa' requires a [device.backend.alsa] table");
  }
  if (device.backend_type == "alsa" && device.part.kind != "mic") {
    Fail(path, where + ": backend type 'alsa' is only implemented for class 'mic'");
  }
  if (device.backend_type == "v4l2" && !device.part.v4l2.has_value() && device.part.kind == "cam") {
    Fail(path, where + ": backend type 'v4l2' requires a [device.backend.v4l2] table");
  }
  if (device.backend_type == "v4l2" && device.part.kind != "cam") {
    Fail(path, where + ": backend type 'v4l2' is only implemented for class 'cam'");
  }
  if (const auto* feeds = tbl.get("feeds"); feeds != nullptr) {
    const auto* table = feeds->as_table();
    if (table == nullptr) {
      Fail(path, where + ": key 'feeds' must be a table");
    }
    device.envelope = ParseFeeds(*table, path, where + " [device.feeds]");
  } else {
    Fail(path, where + ": missing required table 'feeds'");
  }

  if (const auto* primitives = tbl.get("primitive"); primitives != nullptr) {
    const auto* array = primitives->as_array();
    if (array == nullptr) {
      Fail(path, where + ": key 'primitive' must be an array of tables ([[device.primitive]])");
    }
    ParsePrimitives(*array, device, path, where);
  }

  if (device.part.kind == "arm") ValidateArm(device, path, where);

  if (device.part.ros2.has_value()) {
    if (device.part.camera && !tbl["part"]["fov_rad"]) {
      device.part.camera->fov_rad.reset();
    }
    CheckRos2Endpoints(device, path, where);
  }
  return device;
}

}  // namespace detail

using namespace detail;

bool NumericRange::Contains(double value) const {
  if (!std::isfinite(value)) return false;
  const bool above_minimum =
      !minimum.has_value() || (minimum_exclusive ? value > *minimum : value >= *minimum);
  const bool below_maximum =
      !maximum.has_value() || (maximum_exclusive ? value < *maximum : value <= *maximum);
  return above_minimum && below_maximum;
}

const PrimitiveSpec* PrimitiveSpecOf(std::string_view name, std::string_view version) {
  for (const PrimitiveSpec& primitive : kPrimitiveCatalog) {
    if (name == primitive.name && version == primitive.version) return &primitive;
  }
  return nullptr;
}

bool BackendCanExecutePrimitive(const PrimitiveSpec& primitive, std::string_view backend_type) {
  // The loopback arm acknowledges picks as independent simulated commands;
  // unlike a physical deployment it does not couple them to a gripper or
  // perception scene.
  if (primitive.name == std::string_view("arm.pick")) {
    return backend_type == "ros2" || backend_type == "demo";
  }
  if (backend_type == "demo") return true;
  if (primitive.name == std::string_view("face.register")) {
    return backend_type == "v4l2" || backend_type == "ros2";
  }
  if (backend_type == "aplay") {
    return primitive.device_class == std::string_view("speaker") &&
           (primitive.name == std::string_view("speaker.play") ||
            primitive.name == std::string_view("speaker.speak"));
  }
  return backend_type == "ros2" && ((primitive.device_class == std::string_view("arm") &&
                                     (primitive.name == std::string_view("arm.init") ||
                                      primitive.name == std::string_view("arm.movej") ||
                                      primitive.name == std::string_view("arm.movel") ||
                                      primitive.name == std::string_view("arm.pick"))) ||
                                    (primitive.device_class == std::string_view("gimbal") &&
                                     primitive.name == std::string_view("gimbal.motion")) ||
                                    (primitive.device_class == std::string_view("gripper") &&
                                     primitive.name == std::string_view("gripper.motion")));
}

bool IsKnownDevicePrimitive(std::string_view name, std::string_view version) {
  const PrimitiveSpec* primitive = PrimitiveSpecOf(name, version);
  return primitive != nullptr && primitive->tier != PrimitiveTier::kPrivileged;
}

Binding ParseBinding(std::string_view text, std::string path, const std::string& profile) {
  // toml++ throws parse_error with a source-annotated description on
  // malformed text; rethrow with the binding file named first.
  toml::table root;
  try {
    root = toml::parse(text, path);
  } catch (const toml::parse_error& error) {
    throw std::runtime_error("binding '" + path + "': " + std::string(error.description()));
  }

  if (profile.empty()) {
    Fail(path,
         "a profile is required: the [mode] section of etc/eos/eos.toml must name one "
         "[profile.<name>] of this file");
  }
  const auto* profiles = root.get("profile");
  if (profiles == nullptr) {
    Fail(path, "no '[profile.<name>]' tables; cannot select profile '" + profile + "'");
  }
  const auto* profiles_table = profiles->as_table();
  if (profiles_table == nullptr) {
    Fail(path, "key 'profile' must be a table");
  }
  const auto* entry = profiles_table->get(profile);
  if (entry == nullptr) {
    Fail(path, "profile '" + profile + "' not found");
  }
  const auto* scope = entry->as_table();
  if (scope == nullptr) {
    Fail(path, "profile '" + profile + "' must be a table");
  }
  const std::string scope_name = "profile '" + profile + "'";

  static const char* const kProfileKeys[] = {"robot", "device", nullptr};
  RequireKnownKeys(*scope, kProfileKeys, path, scope_name);

  Binding binding;
  binding.path = path;  // kept as a copy: `path` still names the file in errors below
  binding.profile = profile;
  if (const auto* robot = scope->get("robot"); robot != nullptr) {
    const auto* table = robot->as_table();
    if (table == nullptr) {
      Fail(path, scope_name + ": key 'robot' must be a table");
    }
    const std::string where = scope_name + " [robot]";
    if (table->contains("id")) {
      Fail(path, where + ": obsolete key 'id'; rename it to 'robot_id'");
    }
    static const char* const kAllowed[] = {"robot_id",    "robot_name", "robot_desc", "model",
                                           "description", "vendor",     nullptr};
    RequireKnownKeys(*table, kAllowed, path, where);
    binding.robot.id = OptionalString(*table, "robot_id", path, where).value_or("");
    binding.robot.name = OptionalString(*table, "robot_name", path, where).value_or("robot-0");
    binding.robot.model = OptionalString(*table, "model", path, where);
    binding.robot.description = OptionalString(*table, "robot_desc", path, where);
    const auto legacy_description = OptionalString(*table, "description", path, where);
    if (binding.robot.description.has_value() && legacy_description.has_value()) {
      Fail(path, where + ": use only 'robot_desc'; legacy 'description' cannot also be set");
    }
    if (!binding.robot.description.has_value()) binding.robot.description = legacy_description;
    binding.robot.vendor = OptionalString(*table, "vendor", path, where);
  }

  const auto* devices = scope->get("device");
  if (devices == nullptr) {
    Fail(path, scope_name + ": missing required '[[device]]' entries");
  }
  const auto* array = devices->as_array();
  if (array == nullptr) {
    Fail(path, scope_name + ": 'device' must be an array of tables ([[device]])");
  }
  int index = 0;
  for (const auto& device_entry : *array) {
    const auto* table = device_entry.as_table();
    if (table == nullptr) {
      Fail(path, scope_name + ": each 'device' entry must be a table");
    }
    binding.devices.push_back(
        ParseDevice(*table, path, scope_name + " device[" + std::to_string(index) + "]"));
    ++index;
  }
  if (binding.devices.empty()) {
    Fail(path, scope_name + ": '[[device]]' list is empty");
  }
  std::set<std::pair<std::string, std::string>> parts;
  for (const DeviceBinding& device : binding.devices) {
    if (!parts.emplace(device.part.kind, device.part.part_id).second) {
      Fail(path, scope_name + ": duplicate " + device.part.kind + " part_id '" +
                     device.part.part_id + "'");
    }
  }
  for (const DeviceBinding& gimbal : binding.devices) {
    if (gimbal.part.kind != "gimbal" || gimbal.backend_type != "ros2") continue;
    const auto& mapping = *gimbal.part.ros2->gimbal_joint_map;
    const DeviceBinding* arm = nullptr;
    for (const DeviceBinding& candidate : binding.devices) {
      if (candidate.part.kind == "arm" && candidate.backend_type == "ros2" &&
          candidate.part.part_id == mapping.arm_part_id) {
        if (arm != nullptr) {
          Fail(path, scope_name + ": gimbal arm_part_id '" + mapping.arm_part_id +
                         "' matches multiple ROS 2 arms");
        }
        arm = &candidate;
      }
    }
    if (arm == nullptr) {
      Fail(path, scope_name + ": gimbal arm_part_id '" + mapping.arm_part_id +
                     "' does not match a ROS 2 arm");
    }
    const Ros2DeviceConfig& gimbal_ros2 = *gimbal.part.ros2;
    const Ros2DeviceConfig& arm_ros2 = *arm->part.ros2;
    if (gimbal.part.joint_names != arm->part.joint_names) {
      Fail(path, scope_name + ": joint-backed gimbal and arm must use identical joint_names");
    }
    auto same_endpoint = [](const Ros2Endpoint& left, const Ros2Endpoint& right) {
      return left.kind == right.kind && left.name == right.name && left.message == right.message;
    };
    if (gimbal_ros2.domain_id != arm_ros2.domain_id ||
        gimbal_ros2.node_namespace != arm_ros2.node_namespace ||
        gimbal_ros2.telemetry_timeout_ms != arm_ros2.telemetry_timeout_ms ||
        !same_endpoint(gimbal_ros2.joint_states, arm_ros2.joint_states) ||
        !same_endpoint(gimbal_ros2.movej, arm_ros2.movej) ||
        !same_endpoint(gimbal_ros2.stop, arm_ros2.stop) ||
        !same_endpoint(gimbal_ros2.estop, arm_ros2.estop) ||
        !same_endpoint(gimbal_ros2.health, arm_ros2.health)) {
      Fail(path,
           scope_name + ": joint-backed gimbal must share the arm's ROS 2 transport and endpoints");
    }
  }
  return binding;
}

Binding LoadBinding(const std::string& path, const std::string& profile) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("binding '" + path + "': cannot open file for reading");
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return ParseBinding(buffer.str(), path, profile);
}

}  // namespace eos::runtime::hardware_runtime
