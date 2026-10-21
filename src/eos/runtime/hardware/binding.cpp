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
#include "catalog.h"
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
                     const std::string& where, const BackendCatalog& catalog) {
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
    const BackendDescriptor* descriptor = catalog.Find(device.backend_type);
    if (descriptor == nullptr || !descriptor->supports_primitive ||
        !descriptor->supports_primitive(*spec)) {
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
                          const std::string& where, const BackendCatalog& catalog) {
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
    // Ordered URDF joint names define the adapter's command/state channel order.
    // Ranges, types, and velocity ceilings remain authoritative URDF data.
    if (device.part.kind != "arm" && device.part.kind != "gimbal") {
      Fail(path, where + ": key 'joint_names' is only valid for class 'arm' or 'gimbal'");
    }
    device.part.joint_names = OptionalJointNames(tbl, "joint_names", path, where);
  }
  if (const auto* backend = tbl.get("backend"); backend != nullptr) {
    const auto* table = backend->as_table();
    if (table == nullptr) Fail(path, where + ": key 'backend' must be a table");
    const std::string scope = where + " [device.backend]";
    device.backend_type = RequireString(*table, "type", path, scope);
    const BackendDescriptor* descriptor = catalog.Find(device.backend_type);
    if (descriptor == nullptr)
      Fail(path, scope + ": unknown backend type '" + device.backend_type + "'");
    if (!descriptor->supports_class || !descriptor->supports_class(device.part.kind))
      Fail(path, scope + ": backend type '" + device.backend_type +
                     "' does not implement class '" + device.part.kind + "'");
    if (!descriptor->parse)
      Fail(path, scope + ": backend type '" + device.backend_type + "' has no parser");
    device.backend_settings = descriptor->parse(*table, device.part, path, scope);
    if (descriptor->finalize_part) descriptor->finalize_part(device.part, tbl);
    if (!device.backend_settings)
      Fail(path, scope + ": backend type '" + device.backend_type + "' has no settings");
    device.part.camera_frame_limit_bytes = device.backend_settings->CameraFrameLimitBytes();
  } else {
    Fail(path, where + ": missing required table 'backend'");
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
    ParsePrimitives(*array, device, path, where, catalog);
  }

  if (device.part.kind == "arm") ValidateArm(device, path, where);

  const BackendDescriptor* descriptor = catalog.Find(device.backend_type);
  if (descriptor->validate_device) descriptor->validate_device(device, path, where);
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

bool IsKnownDevicePrimitive(std::string_view name, std::string_view version) {
  const PrimitiveSpec* primitive = PrimitiveSpecOf(name, version);
  return primitive != nullptr && primitive->tier != PrimitiveTier::kPrivileged;
}

Binding ParseBinding(std::string_view text, std::string path, const std::string& profile,
                     const BackendCatalog& catalog) {
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
        ParseDevice(*table, path, scope_name + " device[" + std::to_string(index) + "]", catalog));
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
  std::set<std::string> validated_backends;
  for (const DeviceBinding& device : binding.devices) {
    if (!validated_backends.insert(device.backend_type).second) continue;
    const BackendDescriptor* descriptor = catalog.Find(device.backend_type);
    if (descriptor != nullptr && descriptor->validate_binding)
      descriptor->validate_binding(binding, path, scope_name);
  }
  return binding;
}

Binding LoadBinding(const std::string& path, const std::string& profile,
                    const BackendCatalog& catalog) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("binding '" + path + "': cannot open file for reading");
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return ParseBinding(buffer.str(), path, profile, catalog);
}

}  // namespace eos::runtime::hardware_runtime
