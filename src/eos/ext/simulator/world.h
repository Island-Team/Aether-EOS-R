/// Standalone demo driver; the demo backend adapts it to EOS.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "map_store.h"
#include "raster_map.h"
#include "source_media.h"
#include "system_camera_source.h"

namespace eos::ext::simulator {

struct Scene;
struct SimulatorSnapshot;
struct SimulatorRaster;

/// Common mock health payload exposed through every demo adapter.
struct Health {
  bool alive{true};      ///< Whether the simulated device can produce data or accept work.
  bool admitted{false};  ///< Admission is owned by the runtime bridge, not the simulated device.
};

/// Manipulator pose, velocity, joints, and contact state in the base frame.
struct ArmState {
  double ee_position[3]{};
  double ee_orientation_quat[4]{};
  double ee_linear_velocity[3]{};
  double joint_positions_rad[6]{};
  double joint_velocities[6]{};
  bool contact{false};
};

/// Planar mobile-base pose, commanded twist, and driver posture.
struct BaseState {
  double x{0.0}, y{0.0}, yaw{0.0};
  double vx{0.0}, vy{0.0}, yaw_rate{0.0};
  std::uint8_t posture{0};
};

/// Public mock odometry feed. Posture remains a driver-side control state and
/// is intentionally not part of this payload.
struct BaseOdometry {
  double x{0.0}, y{0.0}, yaw{0.0};
  double vx{0.0}, vy{0.0}, yaw_rate{0.0};
};

/// Gimbal angles and rates in radians and radians per second.
struct GimbalState {
  double pan_rad{0.0}, tilt_rad{0.0}, pan_rate{0.0}, tilt_rate{0.0};
};

/// Fixed-size PCM block emitted by the demo microphone.
struct MicrophoneBlock {
  std::uint32_t sample_rate_hz{16000};
  std::uint16_t channels{1};
  std::int16_t samples[64]{};
};

/// Semantic jaw state used by the demo gripper.
enum class GripperJaw : std::uint8_t { kOpen, kClosed, kHolding };
/// Gripper position, force, and optional held-object identity.
struct GripperState {
  GripperJaw state{GripperJaw::kOpen};
  std::string object_held_id;
  double force_n{5.0};
  double position_pct{100.0};
};

/// Demo speaker availability and playback countdown.
struct SpeakerState {
  bool enabled{true}, device_present{true}, playing{false};
  double remaining_s{0.0};
};

/// World-frame object staged in the deterministic demo scene.
struct VisionObject {
  std::string id, label;
  bool pickable{false};
  std::string unpickable_reason;
  double x_m{0.0}, y_m{0.0}, radius_m{0.15}, confidence{0.9};
};

/// Gimbal-relative detection derived from one staged VisionObject.
struct VisionDetection {
  std::string object_id, label;
  bool pickable{false};
  std::string unpickable_reason;
  double distance_m{0.0}, bearing_rad{0.0}, confidence{0.9};
};

/// Immutable simulator state used to correlate a WebUI-rendered camera frame
/// with the exact base/gimbal state from which it was rendered.
struct StateSnapshot {
  std::uint64_t state_sequence{0};
  BaseState base;
  GimbalState gimbal;
  std::uint64_t scene_revision{0};
  std::optional<std::string> selected_line_id;
  std::optional<std::string> selected_source_id;
};

/// One camera-ready JPEG selected and timed entirely by the demo world.
struct CameraSourceFrame {
  std::uint32_t width_px{0};
  std::uint32_t height_px{0};
  std::int64_t source_timestamp_ns{0};
  std::vector<std::uint8_t> jpeg;
  StateSnapshot state;
};

/// Deterministic in-process demo robot.
class World {
 public:
  explicit World(std::vector<VisionObject> objects = {});

  /// Updates the end-effector pose and linear velocity in the base frame.
  void SetCartesian(const double position[3], const double orientation[4],
                    const double velocity[3]);
  /// Sets one zero-based joint position in radians; out-of-range indices are ignored.
  void SetJointPosition(std::size_t index, double radians);
  /// Sets one zero-based joint velocity in radians per second; invalid indices are ignored.
  void SetJointVelocity(std::size_t index, double radians_per_second);
  /// Zeros all manipulator velocities without changing its pose.
  void StopArm();
  ArmState arm() const;

  /// Stores a planar base twist in meters per second and radians per second.
  void DriveBase(double vx, double vy, double yaw_rate);
  /// Applies one robot-relative planar navigation goal immediately.
  void NavigateBaseRelative(double x_m, double y_m, double yaw_rad);
  /// Applies a relative navigation only when the complete swept footprint is free.
  bool NavigateBaseRelativeChecked(double x_m, double y_m, double yaw_rad, double footprint_width_m,
                                   double footprint_length_m);
  /// Replaces the planar pose, used once when a persisted simulation map loads.
  void SetBasePose(double x_m, double y_m, double yaw_rad);
  /// Sets the driver-specific posture value.
  void SetBasePosture(std::uint8_t posture);
  /// Zeros the planar base twist without changing its pose.
  void StopBase();
  BaseState base() const;

  /// Returns the newest immutable state snapshot.
  StateSnapshot CurrentState() const;
  /// Returns a retained snapshot, or nothing when the sequence is unknown or expired.
  std::optional<StateSnapshot> StateAt(std::uint64_t state_sequence) const;
  /// Atomically replaces the active visual-source scene and records its selection.
  void SetScene(Scene scene, const std::filesystem::path& state_root = {},
                double camera_max_range_m = 100.0);
  /// Atomically installs the monitor map, scene, decoded sources, and cached raster.
  void SetSimulator(RasterMap map, Scene scene, const std::filesystem::path& state_root = {},
                    double camera_max_range_m = 100.0);
  /// Returns one internally consistent simulator projection.
  SimulatorSnapshot SimulatorState() const;
  /// Returns the cached canonical raster PNG.
  SimulatorRaster SimulatorRasterPng() const;
  /// Returns the selected source frame for the current demo clock, or nothing
  /// when the camera ray hits no line.
  std::optional<CameraSourceFrame> ReadCameraSource() const;
  /// Overrides scene-selected media with one continuous camera source. Passing
  /// null restores scene selection.
  void SetCameraSource(std::shared_ptr<CameraFrameSource> source);

  /// Sets gimbal angles and rates in radians and radians per second.
  void PointGimbal(double pan, double tilt, double pan_rate, double tilt_rate);
  /// Zeros gimbal rates without changing its angles.
  void StopGimbal();
  /// Recomputes detections relative to the supplied world-frame base pose.
  void UpdateVision(double base_x, double base_y, double base_yaw);
  GimbalState gimbal() const;
  std::vector<VisionObject> objects() const;
  std::vector<VisionDetection> detections() const;
  /// Removes the object with `id`, returning false when it is absent.
  bool RemoveObject(const std::string& id);
  /// Adds an object and refreshes detections; the caller owns ID uniqueness.
  void AddObject(VisionObject object);

  /// Stages an object ID for the next closing gripper command.
  void ArmGrasp(std::string object_id);
  /// Opens or closes the gripper with the requested force in newtons.
  void MoveGripper(bool close, double force_n);
  /// Zeros applied gripper force while preserving jaw state.
  void StopGripper();
  GripperState gripper() const;

  /// Starts simulated audio playback with duration derived from encoded bytes.
  void PlayAudio(std::size_t bytes);
  /// Stops simulated playback.
  void StopSpeaker();
  SpeakerState speaker() const;

  /// Advances base integration and speaker playback by `dt_s` seconds.
  void Tick(double dt_s);
  MicrophoneBlock microphone() const;

  /// Irreversibly places this mock device in its safe envelope.
  void Halt();
  /// Reports whether Halt has not been latched.
  bool available() const;
  /// Returns the common health projection for this world.
  Health health() const;

 private:
  void RecordStateLocked();
  void Detect();
  const DecodedMedia* LoadSelectedSourceLocked(const std::string& source_id) const;
  mutable std::mutex mutex_;
  ArmState arm_{};
  BaseState base_{};
  GimbalState gimbal_{};
  MicrophoneBlock microphone_{};
  GripperState gripper_{};
  SpeakerState speaker_{};
  std::vector<VisionObject> objects_;
  std::vector<VisionDetection> detections_;
  std::string armed_grasp_id_;
  double vision_base_x_{0.0}, vision_base_y_{0.0}, vision_base_yaw_{0.0};
  std::uint64_t next_state_sequence_{1};
  std::shared_ptr<const Scene> scene_;
  RasterMapMetadata simulator_map_;
  std::shared_ptr<const RasterMap> raster_map_;
  std::vector<std::uint8_t> simulator_raster_png_;
  std::filesystem::path source_state_root_;
  mutable std::deque<DecodedMedia> source_cache_;
  mutable std::size_t source_cache_bytes_{0};
  std::shared_ptr<CameraFrameSource> camera_source_;
  double camera_max_range_m_{100.0};
  double session_elapsed_s_{0.0};
  std::deque<StateSnapshot> state_history_;
  bool halted_{false};
};

}  // namespace eos::ext::simulator
