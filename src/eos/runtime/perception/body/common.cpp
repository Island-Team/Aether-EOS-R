/// Wire projection: one builder per north-bound resource. Backend-neutral
/// telemetry plus PartConfig in, contract-shaped JSON out (schemas:
/// share/contracts/eos/body, shapes: docs/island/contracts_aether.md). This translation
/// unit only reads admitted devices; every command path lives in dispatch.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "detail.h"
#include "reader.h"
#include "src/eos/runtime/hardware/registry.h"
#include "src/eos/runtime/models/microphone_vad.h"

namespace eos::runtime::perception::body {
namespace detail {

using hardware::DeviceKind;
using hardware_runtime::PartConfig;

/// One per-part resource family: the contract URI segment, the binding part
/// kind it is fed from, and the device kind behind it. The single table
/// behind SegmentOfKind() —
/// binding kinds "arm"/"cam"/"mic" spell
/// "manipulator"/"camera"/"microphone" on the
/// wire.
struct Family {
  const char* segment;
  const char* binding_kind;
  DeviceKind device_kind;
};

const Family kFamilies[] = {
    {"base", "base", DeviceKind::kBase},
    {"gimbal", "gimbal", DeviceKind::kGimbal},
    {"manipulator", "arm", DeviceKind::kManipulator},
    {"camera", "cam", DeviceKind::kCamera},
    {"microphone", "mic", DeviceKind::kMicrophone},
    {"gripper", "gripper", DeviceKind::kGripper},
    {"speaker", "speaker", DeviceKind::kSpeaker},
};

/// First device of `kind` whose part kind string is `binding_kind` and whose
/// part carries `part_id`, or nullptr. Mirrors the registry's own DemoOf
/// double gate (backend kind tag plus part kind string plus part id) so the
/// config and the backend always come from the same admitted device.
const hardware_runtime::Device* FindDevice(const hardware_runtime::Registry& registry,
                                           DeviceKind kind, const char* binding_kind,
                                           const std::string& part_id) {
  for (const hardware_runtime::Device& device : registry.devices()) {
    if (device.identity().device_id == part_id && device.identity().kind == kind &&
        device.part().kind == binding_kind) {
      return &device;
    }
  }
  for (const hardware_runtime::Device& device : registry.devices()) {
    if (device.identity().kind == kind && device.part().kind == binding_kind &&
        device.part().part_id == part_id) {
      return &device;
    }
  }
  return nullptr;
}

/// First device of `kind` mounted on `robot_id`, or nullptr.
const hardware_runtime::Device* FindRobotDevice(const hardware_runtime::Registry& registry,
                                                const std::string& robot_id, DeviceKind kind) {
  for (const hardware_runtime::Device& device : registry.devices()) {
    if (device.robot_id() == robot_id && device.identity().kind == kind) {
      return &device;
    }
  }
  return nullptr;
}

DeviceStatus ProjectDeviceStatusImpl(const hardware_runtime::Device& device) {
  const hardware_runtime::DeviceHealth health = device.ReadHealth();
  if (!health.alive) return {"fault", "unavailable"};
  if (!health.admitted) return {"disabled", "degraded"};

  std::string state = "unknown";
  constexpr double kMovingEpsilon = 1e-9;
  if (device.part().kind == "base") {
    const auto telemetry = device.feeds().ReadBaseTelemetry();
    if (telemetry) {
      state = std::abs(telemetry->linear_x_m_s) > kMovingEpsilon ||
                      std::abs(telemetry->linear_y_m_s) > kMovingEpsilon ||
                      std::abs(telemetry->angular_z_rad_s) > kMovingEpsilon
                  ? "active"
                  : "idle";
    }
  } else if (device.part().kind == "gimbal") {
    const auto telemetry = device.feeds().ReadGimbalTelemetry();
    if (telemetry) {
      state = std::abs(telemetry->pan_rate_rad_s) > kMovingEpsilon ||
                      std::abs(telemetry->tilt_rate_rad_s) > kMovingEpsilon
                  ? "active"
                  : "idle";
    }
  } else if (device.part().kind == "arm") {
    const auto telemetry = device.feeds().ReadManipulatorTelemetry();
    if (telemetry) {
      bool has_sample = telemetry->pose_valid;
      bool moving = false;
      for (std::size_t index = 0; index < telemetry->joint_valid.size(); ++index) {
        if (!telemetry->joint_valid[index]) continue;
        has_sample = true;
        moving = moving || std::abs(telemetry->joint_velocities_rad_s[index]) > kMovingEpsilon;
      }
      if (has_sample) state = moving ? "active" : "idle";
    }
  } else if (device.part().kind == "cam") {
    const auto frame = device.feeds().ReadCameraTelemetry();
    if (frame && !frame->jpeg.empty()) state = "capturing";
  } else if (device.part().kind == "mic") {
    // Microphone telemetry is a destructive stream read; the VAD pump is its sole consumer.
    state = "listening";
  } else if (device.part().kind == "gripper") {
    const auto gripper = device.feeds().ReadGripperTelemetry();
    if (gripper && gripper->state != hardware::GripperState::kUnknown) state = "idle";
  } else if (device.part().kind == "speaker") {
    const auto speaker = device.feeds().ReadSpeakerTelemetry();
    if (speaker) {
      switch (speaker->state) {
        case hardware::SpeakerPlaybackState::kBuffering:
          state = "buffering";
          break;
        case hardware::SpeakerPlaybackState::kPlaying:
        case hardware::SpeakerPlaybackState::kDraining:
          state = "active";
          break;
        case hardware::SpeakerPlaybackState::kFault:
          state = "fault";
          break;
        case hardware::SpeakerPlaybackState::kIdle:
          state = "idle";
          break;
      }
    }
  }
  return {std::move(state), "ready"};
}

nlohmann::json DeviceSummaryOf(const hardware_runtime::Device& device, const char* device_class) {
  const DeviceStatus status = ProjectDeviceStatusImpl(device);
  return {{"device_class", device_class}, {"state", status.state}, {"health", status.health}};
}

/// RFC 4648 base64 of `bytes` (standard alphabet, padded).
std::string Base64Encode(const std::uint8_t* bytes, std::size_t size) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(4 * ((size + 2) / 3));
  for (std::size_t i = 0; i < size; i += 3) {
    const std::uint32_t triple = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                 (i + 1 < size ? bytes[i + 1] : 0) << 8 |
                                 (i + 2 < size ? bytes[i + 2] : 0);
    out.push_back(kAlphabet[(triple >> 18) & 0x3f]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3f]);
    out.push_back(i + 1 < size ? kAlphabet[(triple >> 6) & 0x3f] : '=');
    out.push_back(i + 2 < size ? kAlphabet[triple & 0x3f] : '=');
  }
  return out;
}

/// Wire literal of a base posture.
const char* PostureOf(hardware::BasePosture posture) {
  switch (posture) {
    case hardware::BasePosture::kStand:
      return "stand";
    case hardware::BasePosture::kWalk:
      return "walk";
    case hardware::BasePosture::kLie:
      return "lie";
  }
  return "stand";
}

/// Wire literal of a jaw state.
const char* JawOf(hardware::GripperState jaw) {
  switch (jaw) {
    case hardware::GripperState::kUnknown:
      return "unknown";
    case hardware::GripperState::kOpen:
      return "open";
    case hardware::GripperState::kClosed:
      return "closed";
    case hardware::GripperState::kHolding:
      return "holding";
  }
  return "unknown";
}

/// RFC3339 UTC timestamp ("2026-08-16T09:15:32Z") of the clock's wall read.
std::string TimestampOf(std::chrono::system_clock::time_point time) {
  const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  char text[24];
  std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return text;
}

std::string TimestampOf(const lib::Clock& clock) { return TimestampOf(clock.WallNow()); }

/// Inner BaseState struct shared by base.snapshot and the scene aggregate.
nlohmann::json BaseStateOf(const PartConfig& part, const hardware::BaseTelemetry& telemetry) {
  const hardware_runtime::BasePartConfig geometry =
      part.base.value_or(hardware_runtime::BasePartConfig{});
  return {
      {"pose", {{"x_m", telemetry.x_m}, {"y_m", telemetry.y_m}, {"yaw_rad", telemetry.yaw_rad}}},
      {"velocity",
       {{"x_m_s", telemetry.linear_x_m_s},
        {"y_m_s", telemetry.linear_y_m_s},
        {"yaw_rad_s", telemetry.angular_z_rad_s}}},
      {"posture", PostureOf(telemetry.posture)},
      {"footprint_length_m", geometry.footprint_length_m},
      {"footprint_width_m", geometry.footprint_width_m},
  };
}

/// Inner GimbalState struct shared by gimbal.snapshot and the scene aggregate.
/// The contract gimbal is single-axis: pan projects to yaw; the demo's tilt
/// channel has no wire slot and is deliberately dropped here.
nlohmann::json GimbalStateOf(const PartConfig& part, const hardware::GimbalTelemetry& telemetry) {
  const hardware_runtime::GimbalPartConfig geometry =
      part.gimbal.value_or(hardware_runtime::GimbalPartConfig{});
  return {
      {"yaw_rad", telemetry.pan_rad},
      {"yaw_rate_rad_s", telemetry.pan_rate_rad_s},
      {"fov_rad", geometry.fov_rad},
      {"range_m", geometry.range_m},
  };
}

/// Inner ManipulatorState struct shared by manipulator.snapshot and the scene
/// aggregate. Joints are the source of truth: joint_positions_rad fill the
/// configured link slots and the trailing slots pad with null; `end` is planar
/// forward kinematics over those joints (x = mount + sum link*cos(cumulative
/// angle), y analog, yaw = summed angles; z/roll/pitch null). The demo's
/// south-bound ee pose is not projected — it disagrees with the joints by
/// construction, and the contract derives the end pose from the chain.
/// Projects measured spatial-arm telemetry into the v1 shape. V1 retains
/// Euler fields, so the ROS quaternion is converted only at this wire edge.
nlohmann::json ManipulatorStateOf(const PartConfig& part,
                                  const hardware::ManipulatorTelemetry& telemetry) {
  const hardware_runtime::ArmPartConfig geometry =
      part.arm.value_or(hardware_runtime::ArmPartConfig{});
  nlohmann::json joints = nlohmann::json::array();
  nlohmann::json links = nlohmann::json::array();
  for (std::size_t index = 0; index < telemetry.joint_angles_rad.size(); ++index) {
    joints.push_back(telemetry.joint_valid[index]
                         ? nlohmann::json(telemetry.joint_angles_rad[index])
                         : nlohmann::json(nullptr));
    links.push_back(geometry.link_lengths_m[index].has_value()
                        ? nlohmann::json(*geometry.link_lengths_m[index])
                        : nlohmann::json(nullptr));
  }

  nlohmann::json end = {
      {"x_m", nullptr},     {"y_m", nullptr},      {"z_m", nullptr},
      {"yaw_rad", nullptr}, {"roll_rad", nullptr}, {"pitch_rad", nullptr},
  };
  if (telemetry.pose_valid) {
    const double x = telemetry.orientation_quat[0];
    const double y = telemetry.orientation_quat[1];
    const double z = telemetry.orientation_quat[2];
    const double w = telemetry.orientation_quat[3];
    const double pitch_input = std::clamp(2.0 * (w * y - z * x), -1.0, 1.0);
    end = {
        {"x_m", telemetry.position_m[0]},
        {"y_m", telemetry.position_m[1]},
        {"z_m", telemetry.position_m[2]},
        {"yaw_rad", std::atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z))},
        {"roll_rad", std::atan2(2.0 * (w * x + y * z), 1.0 - 2.0 * (x * x + y * y))},
        {"pitch_rad", std::asin(pitch_input)},
    };
  }
  return {
      {"mount_x_m", geometry.mount_x_m},
      {"mount_y_m", geometry.mount_y_m},
      {"joint_angles_rad", std::move(joints)},
      {"link_lengths_m", std::move(links)},
      {"end", std::move(end)},
  };
}

/// Splits `text` on '/'.
std::vector<std::string> SplitUri(const std::string& text) {
  std::vector<std::string> parts;
  std::istringstream stream(text);
  std::string part;
  while (std::getline(stream, part, '/')) {
    parts.push_back(part);
  }
  return parts;
}

/// Throws the absent-part error shared by every per-part builder.
[[noreturn]] void ThrowAbsentPart(const char* segment, const std::string& part_id) {
  throw std::runtime_error(std::string("state projection: no ") + segment + " part '" + part_id +
                           "' in the registry");
}

}  // namespace detail

using namespace detail;

DeviceStatus ProjectDeviceStatus(const hardware_runtime::Device& device) {
  return ProjectDeviceStatusImpl(device);
}

const char* SegmentOfKind(const std::string& kind) {
  for (const Family& family : kFamilies) {
    if (kind == family.binding_kind) {
      return family.segment;
    }
  }
  return nullptr;
}

Reader::Reader(const hardware_runtime::Registry& registry, const lib::Clock& clock,
               const models::CameraPoseStore* camera_pose,
               const models::CameraObjectStore* camera_object,
               const models::MicrophoneVadStore* microphone_vad,
               const DetectionOverlayStore* detection_overlay,
               const store::FurnitureRegionStore* furniture_regions,
               std::string default_object_camera_device_id)
    : registry_(registry),
      clock_(clock),
      camera_pose_(camera_pose),
      camera_object_(camera_object),
      microphone_vad_(microphone_vad),
      detection_overlay_(detection_overlay),
      furniture_regions_(furniture_regions),
      default_object_camera_device_id_(std::move(default_object_camera_device_id)),
      start_mono_(clock.MonotonicNow()) {}

nlohmann::json Reader::StartEnvelope(const char* type, const char* frame_id,
                                     const std::string& part_id, const PartConfig* part,
                                     const std::string& counter_key) {
  const std::uint64_t sequence = ++sequence_[counter_key];
  return {
      {"type", type},
      {"schema_version", "1.0"},
      {"sequence", sequence},
      {"timestamp", TimestampOf(clock_)},
      {"frame_id", frame_id},
      {"part_id", part_id},
      {"description", part != nullptr && part->description.has_value()
                          ? nlohmann::json(*part->description)
                          : nlohmann::json(nullptr)},
      {"tags", part != nullptr ? nlohmann::json(part->tags) : nlohmann::json::array()},
  };
}

}  // namespace eos::runtime::perception::body
