/// Read-only body, detection, and speech projection from admitted feeds.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

#include "src/eos/lib/clock.h"
#include "src/eos/runtime/models/camera_object.h"

namespace eos::runtime::hardware_runtime {
class Device;
class Registry;
struct PartConfig;
}  // namespace eos::runtime::hardware_runtime
namespace eos::runtime::models {
class CameraObjectStore;
class CameraPoseStore;
class MicrophoneVadStore;
}  // namespace eos::runtime::models
namespace eos::runtime::store {
class FurnitureRegionStore;
}

namespace eos::runtime::perception::body {
class DetectionOverlayStore;

struct DeviceStatus {
  std::string state;
  std::string health;
};

/// Projects backend-neutral health and telemetry without inventing activity.
DeviceStatus ProjectDeviceStatus(const hardware_runtime::Device& device);

/// Returns the contract segment for a part kind, or nullptr if unknown.
const char* SegmentOfKind(const std::string& kind);

/// Registry and clock must outlive Reader; unavailable semantic data throws.
class Reader {
 public:
  Reader(const hardware_runtime::Registry& registry, const lib::Clock& clock,
         const models::CameraPoseStore* camera_pose = nullptr,
          const models::CameraObjectStore* camera_object = nullptr,
          const models::MicrophoneVadStore* microphone_vad = nullptr,
          const DetectionOverlayStore* detection_overlay = nullptr,
          const store::FurnitureRegionStore* furniture_regions = nullptr,
          std::string default_object_camera_device_id = {});

  /// Compact mobile-base health and state.
  nlohmann::json BuildBase(const std::string& part_id = "default");

  /// Normalized mobile-base telemetry and operational status for one robot.
  nlohmann::json BuildBaseStatus(const std::string& robot_id);

  /// Compact gimbal health and state.
  nlohmann::json BuildGimbal(const std::string& part_id = "default");

  /// Compact manipulator health and state.
  nlohmann::json BuildManipulator(const std::string& part_id = "default");

  /// Latest joint telemetry for the robot's manipulator.
  nlohmann::json BuildArmState(const std::string& robot_id);

  /// Normalized manipulator telemetry and operational status for one robot.
  nlohmann::json BuildArmStatus(const std::string& robot_id);

  /// Compact camera health and state; contains no image or model result.
  nlohmann::json BuildCamera(const std::string& part_id = "default");

  /// Compact microphone health and state; contains no audio or model result.
  nlohmann::json BuildMic(const std::string& part_id = "default");

  /// Compact gripper health and state.
  nlohmann::json BuildGripper(const std::string& part_id = "default");

  /// Normalized gripper telemetry and operational status for one robot.
  nlohmann::json BuildGripperStatus(const std::string& robot_id);

  /// Compact speaker health and state; contains no audio details.
  nlohmann::json BuildSpeaker(const std::string& part_id = "default");

  /// Semantic detections for the process's robot, without image geometry.
  nlohmann::json BuildDetection();

  /// Semantic detections scoped to one admitted robot.
  nlohmann::json BuildDetection(const std::string& robot_id);

  /// Object detections from the configured default object camera.
  nlohmann::json BuildHeadDetection(const std::string& robot_id);
  nlohmann::json BuildCameraObjectDetection(const std::string& robot_id,
                                            const std::string& camera_device_id);
  nlohmann::json ProjectCameraObjectDetection(
      const std::string& robot_id,
      const models::CameraObjectStore::Snapshot& object_snapshot);

  /// Compact recognized speech containing no VAD or token timing.
  nlohmann::json BuildAsr();

  /// Compact voice activity state for the latest processed microphone window.
  nlohmann::json BuildVad();

  /// Runtime inventory at `robots` or `robots/{robot_name}/devices`.
  nlohmann::json BuildDevices();

  /// Class-specific body contract object for one process-unique device ID.
  nlohmann::json BuildDeviceState(const std::string& robot_id, const std::string& device_id);

  /// Admitted device states and semantic resources currently available for reads.
  nlohmann::json ListResources();

  /// Per-robot status summaries used by `eos/status`.
  nlohmann::json BuildRobotOverviews();

  /// Dispatches the currently produced resource URIs.
  /// Throws std::runtime_error naming the URI
  /// (and the recognized families) when it matches no resource — the
  /// signaling choice mirrors Registry::Find.
  /// `log_success` is false for watch sampling so successful reads and
  /// expected unavailable samples do not perform synchronous log I/O;
  /// unexpected projection failures are always logged.
  nlohmann::json BuildByUri(const std::string& uri, bool log_success = true);

  /// True when a skill resource template names a route this Reader owns.
  bool SupportsResourceTemplate(const std::string& uri_template) const;

 private:
  /// Starts a snapshot: envelope fields plus the bumped per-resource
  /// sequence. `part` carries description/tags when the resource has a part;
  /// pass nullptr for aggregates (description null, tags empty).
  nlohmann::json StartEnvelope(const char* type, const char* frame_id, const std::string& part_id,
                               const hardware_runtime::PartConfig* part,
                               const std::string& counter_key);

  const hardware_runtime::Registry& registry_;
  const lib::Clock& clock_;
  const models::CameraPoseStore* camera_pose_;
  const models::CameraObjectStore* camera_object_;
  const models::MicrophoneVadStore* microphone_vad_;
  const DetectionOverlayStore* detection_overlay_;
  const store::FurnitureRegionStore* furniture_regions_;
  std::string default_object_camera_device_id_;
  lib::MonotonicTime start_mono_;                  ///< uptime origin, projection-owned
  std::map<std::string, std::uint64_t> sequence_;  ///< per-resource monotonic counter
  std::mutex resource_mutex_;  ///< serializes URI reads from gateway and skill hosts
};

}  // namespace eos::runtime::perception::body
