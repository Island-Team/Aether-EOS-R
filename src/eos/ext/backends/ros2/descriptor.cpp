#include "descriptor.h"

#include <memory>
#include <utility>

#include "src/eos/ext/backends/detail/config_parser.h"
#include "src/eos/ext/backends/ros2/config.h"
#if defined(EOS_BACKEND_ROS2)
#include "src/eos/ext/backends/ros2/ros2_backend.h"
#endif
namespace eos::backends {
using namespace eos::runtime::hardware_runtime;
using namespace eos::backends::detail;
using namespace eos::backends::config;
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

struct Ros2Settings final : BackendSettings {
  Ros2DeviceConfig value;
  std::uint64_t CameraFrameLimitBytes() const override { return value.max_image_bytes; }
};
void CheckRos2Endpoints(const DeviceBinding& device, const std::string& path,
                        const std::string& where) {
  const Ros2DeviceConfig& ros2 = static_cast<const Ros2Settings&>(*device.backend_settings).value;
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

void ValidateRos2Device(const DeviceBinding& device, const std::string& path,
                        const std::string& where) {
  const auto& settings = static_cast<const Ros2Settings&>(*device.backend_settings);
  const auto& config = settings.value;
  if (device.part.kind == "gimbal") {
    if (device.part.joint_names.size() != 6)
      Fail(path, where + ": ROS 2 joint-backed gimbal requires exactly six joint_names");
    if (!config.gimbal_joint_map)
      Fail(path, where + ": ROS 2 gimbal requires [device.backend.ros2.gimbal_joint_map]");
    for (const std::string* name :
         {&config.gimbal_joint_map->pan_joint, &config.gimbal_joint_map->tilt_joint}) {
      if (std::find(device.part.joint_names.begin(), device.part.joint_names.end(), *name) ==
          device.part.joint_names.end())
        Fail(path, where + ": mapped gimbal joint '" + *name + "' is not in joint_names");
    }
  } else if (config.gimbal_joint_map) {
    Fail(path, where + ": gimbal_joint_map is only valid for a ROS 2 gimbal");
  }
  CheckRos2Endpoints(device, path, where);
}
void ValidateRos2Binding(const Binding& binding, const std::string& path,
                         const std::string& where) {
  for (const DeviceBinding& gimbal : binding.devices) {
    if (gimbal.part.kind != "gimbal" || gimbal.backend_type != "ros2") continue;
    const auto& gc = static_cast<const Ros2Settings&>(*gimbal.backend_settings).value;
    const auto& mapping = *gc.gimbal_joint_map;
    const DeviceBinding* arm = nullptr;
    for (const DeviceBinding& candidate : binding.devices) {
      if (candidate.part.kind == "arm" && candidate.backend_type == "ros2" &&
          candidate.part.part_id == mapping.arm_part_id) {
        if (arm)
          Fail(path, where + ": gimbal arm_part_id '" + mapping.arm_part_id +
                         "' matches multiple ROS 2 arms");
        arm = &candidate;
      }
    }
    if (!arm)
      Fail(path,
           where + ": gimbal arm_part_id '" + mapping.arm_part_id + "' does not match a ROS 2 arm");
    const auto& ac = static_cast<const Ros2Settings&>(*arm->backend_settings).value;
    if (gimbal.part.joint_names != arm->part.joint_names)
      Fail(path, where + ": joint-backed gimbal and arm must use identical joint_names");
    auto same = [](const Ros2Endpoint& a, const Ros2Endpoint& b) {
      return a.kind == b.kind && a.name == b.name && a.message == b.message;
    };
    if (gc.domain_id != ac.domain_id || gc.node_namespace != ac.node_namespace ||
        gc.telemetry_timeout_ms != ac.telemetry_timeout_ms ||
        !same(gc.joint_states, ac.joint_states) || !same(gc.movej, ac.movej) ||
        !same(gc.stop, ac.stop) || !same(gc.estop, ac.estop) || !same(gc.health, ac.health))
      Fail(path,
           where + ": joint-backed gimbal must share the arm's ROS 2 transport and endpoints");
  }
}

BackendDescriptor Ros2Descriptor() {
  BackendDescriptor d;
  d.type = "ros2";
#if !defined(EOS_BACKEND_ROS2)
  d.compiled = false;
#endif
  d.supports_class = [](std::string_view kind) {
    return Supports(kind, {"arm", "gimbal", "gripper", "cam"});
  };
  d.parse = [](const toml::table& tbl, const PartConfig& part, const std::string& path,
               const std::string& where) -> std::shared_ptr<const BackendSettings> {
    const auto* node = OneTable(tbl, "ros2", true, path, where);
    auto settings = std::make_shared<Ros2Settings>();
    settings->value = ParseRos2Table(*node, path, where + " [device.backend.ros2]");
    if (part.kind != "gimbal" && settings->value.gimbal_joint_map)
      Fail(path, where + ": gimbal_joint_map is only valid for a ROS 2 gimbal");
    return settings;
  };
  d.finalize_part = [](PartConfig& part, const toml::table& device_table) {
    if (part.kind != "cam" || !part.camera) return;
    const auto* geometry = device_table.get("part");
    if (!geometry || !geometry->as_table()->contains("fov_rad")) part.camera->fov_rad.reset();
  };
  d.supports_primitive = [](const PrimitiveSpec& p) {
    return HasPrimitive(p, {"arm.init", "arm.movej", "arm.movel", "arm.pick", "gimbal.motion",
                            "gripper.motion", "face.register"});
  };
  d.validate_device = ValidateRos2Device;
  d.validate_binding = ValidateRos2Binding;
  d.create = [](std::string id, const PartConfig& part,
                const BackendSettings& settings) -> std::unique_ptr<hardware::Backend> {
#if defined(EOS_BACKEND_ROS2)
    const auto& value = static_cast<const Ros2Settings&>(settings).value;
    hardware::DeviceKind kind = hardware::DeviceKind::kCamera;
    if (part.kind == "arm")
      kind = hardware::DeviceKind::kManipulator;
    else if (part.kind == "gimbal")
      kind = hardware::DeviceKind::kGimbal;
    else if (part.kind == "gripper")
      kind = hardware::DeviceKind::kGripper;
    return std::make_unique<ros2::Ros2Backend>(std::move(id), value, kind, part.joint_names);
#else
    (void)id;
    (void)part;
    (void)settings;
    return nullptr;
#endif
  };
  return d;
}

}  // namespace eos::backends
