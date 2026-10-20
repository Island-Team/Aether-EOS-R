/// Internal south-bound adapter contract.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eos::hardware {

/// Zero disables the feed.
struct FeedEnvelope {
  double max_rate_hz{0.0};
};

/// Feed rates declared at admission; sensor data excludes model inference.
struct FeedDeclarations {
  FeedEnvelope geometric;       ///< raw geometry such as depth or point samples
  FeedEnvelope proprioception;  ///< joint state, EE pose, F/T, contact events
  FeedEnvelope sensor;          ///< raw/minimally normalized sensor observations
  FeedEnvelope health;          ///< identity + liveness for bridge and Dispatcher
};

/// Joint targets use the device's configured order and radians.
struct MoveJCommand {
  std::vector<double> joint_positions_rad;
  std::optional<double> speed_rad_s;  ///< positive rad/s; nullopt = backend default
  std::chrono::steady_clock::time_point stamp;
  std::string command_id;
  bool stream{false};  ///< true = accept after release without awaiting terminal completion
};

/// Backend resolves IK, collision checks, and trajectory generation.
struct MoveLCommand {
  double position_m[3]{};
  double orientation_quat_xyzw[4]{0.0, 0.0, 0.0, 1.0};
  std::optional<double> speed_m_s;  ///< positive m/s; nullopt = backend default
  std::chrono::steady_clock::time_point stamp;
  std::string command_id;
  bool stream{false};  ///< true = accept after release without awaiting terminal completion
};

/// Object ID for backend-side lookup, planning, and physical completion.
struct PickCommand {
  std::string object_id;
  std::chrono::steady_clock::time_point stamp;
  std::string command_id;
};

/// Planar velocity in SI units; backend owns odometry and motor control.
struct BaseCommand {
  double linear_x_m_s{0.0};
  double linear_y_m_s{0.0};
  double angular_z_rad_s{0.0};
  std::chrono::steady_clock::time_point stamp;
  std::string command_id;
};

/// Relative x/y/yaw goal; absolute targets use stream_map_target.
/// Backend owns localization, planning, and completion.
struct BaseNavigateRelativeCommand {
  double x_m{0.0};
  double y_m{0.0};
  double yaw_rad{0.0};
  std::chrono::steady_clock::time_point stamp;
  std::string command_id;
  bool stream{false};  ///< true = accept after release without awaiting terminal completion
  /// Fixed map-frame target for stream following or ordinary absolute navigation.
  std::optional<std::array<double, 3>> stream_map_target;
  bool absolute{false};  ///< use the map target for ordinary absolute navigation
  std::optional<double> z_m;  ///< map-frame height; absent uses current localized height
  std::optional<double> speed;  ///< vendor speed 0=normal, 1=low, 2=high; absent uses low
};

/// Pan/tilt target in radians; backend owns stabilization.
struct GimbalCommand {
  double pan_rad{0.0};
  double tilt_rad{0.0};
  std::optional<double> speed_rad_s;  ///< positive rad/s; nullopt = backend default
  std::chrono::steady_clock::time_point stamp;
  std::string command_id;
  bool stream{false};  ///< true = accept after release without awaiting terminal completion
};

/// Gripper action released south-bound: open the jaws or close on a grasp.
enum class GripperAction : std::uint8_t {
  kOpen,
  kClose,
};

/// Backend owns force control and grasp logic.
struct GripperCommand {
  GripperAction action{GripperAction::kClose};
  std::optional<double> force_n;      ///< positive newtons; nullopt = backend default
  std::optional<double> opening_pct;  ///< v2: 0 closed or 100 open on Inspire
  std::optional<double> speed_pct;    ///< v2: controller speed ceiling, 1..100 percent
  std::chrono::steady_clock::time_point stamp;
  std::string command_id;
};

/// Complete encoded asset; the backend accepts or rejects it atomically.
struct SpeakerCommand {
  std::string asset_id;            ///< EOS-owned non-empty diagnostic identity
  std::string media_type;          ///< MIME type, currently audio/wav
  std::vector<std::uint8_t> data;  ///< complete bounded encoded audio asset
  std::chrono::steady_clock::time_point stamp;
};

/// Backend owns posture trajectory and actuator control.
enum class BasePosture : std::uint8_t {
  kStand,
  kWalk,
  kLie,
};

struct BasePostureCommand {
  BasePosture posture{BasePosture::kStand};
  std::string command_id;
};

/// Device class selects the permitted release path.
enum class DeviceKind : std::uint8_t {
  kManipulator,  ///< MoveJCommand, MoveLCommand; e.g. robotic arm
  kBase,         ///< BaseCommand; mobile base
  kGimbal,       ///< GimbalCommand; pan/tilt mount
  kCamera,       ///< feed-only; rejects every release
  kMicrophone,   ///< feed-only; rejects every release
  kGripper,      ///< GripperCommand; end-effector jaws
  kSpeaker,      ///< SpeakerCommand; audio output device
};

struct BackendIdentity {
  std::string name;                           ///< adapter identity, e.g. "demo" or "ros2"
  std::string version;                        ///< adapter contract version
  std::string device_id;                      ///< deployment-specific device handle
  DeviceKind kind{DeviceKind::kManipulator};  ///< device class; selects release path
};

struct BackendHealth {
  bool alive{true};
  bool admitted{false};
};

/// Raw base telemetry; north-bound projection selects semantic fields.
struct BaseTelemetry {
  double x_m{0.0};
  double y_m{0.0};
  double yaw_rad{0.0};
  double linear_x_m_s{0.0};
  double linear_y_m_s{0.0};
  double angular_z_rad_s{0.0};
  BasePosture posture{BasePosture::kStand};
  /// Wall-clock observation time supplied by the adapter. A default value
  /// means the source does not provide status freshness metadata.
  std::chrono::system_clock::time_point observed_at{};
  /// Status samples may retain the last known measurements after a polling
  /// failure. Consumers must check these flags before using them for control.
  bool valid{true};
  bool fresh{true};
  bool pose_valid{true};
  bool localization_valid{false};
  std::optional<double> z_m;
  std::optional<double> roll_rad;
  std::optional<double> pitch_rad;
  std::optional<int> battery_percent;
  std::optional<double> forward_speed_m_s;
  std::optional<double> current_odometry_m;
  std::optional<double> total_odometry_m;
  std::optional<double> map_resolution_m_per_pixel;
  std::optional<double> map_origin_x_m;
  std::optional<double> map_origin_y_m;
  std::optional<std::int64_t> map_height_pixels;
  /// Opaque backend status values. Keys are stable adapter-level names; their
  /// numeric meanings remain vendor-defined unless separately normalized.
  std::map<std::string, std::int64_t> diagnostics;
};

struct GimbalTelemetry {
  double pan_rad{0.0};
  double tilt_rad{0.0};
  double pan_rate_rad_s{0.0};
  double tilt_rate_rad_s{0.0};
};

/// Six slots match the v1 contract; joint_valid distinguishes missing from zero.
struct ManipulatorJointDiagnostics {
  std::optional<bool> enabled;
  std::optional<std::int64_t> error_code;
  /// Vendor units are unspecified.
  std::optional<double> motor_current_raw;
  std::optional<double> temperature_raw;
  std::optional<double> voltage_raw;
};

struct ManipulatorTelemetry {
  std::array<double, 6> joint_angles_rad{};
  std::array<double, 6> joint_velocities_rad_s{};
  std::array<bool, 6> joint_valid{};
  std::array<bool, 6> joint_velocity_valid{};
  double position_m[3]{};
  double orientation_quat[4]{0.0, 0.0, 0.0, 1.0};  ///< x, y, z, w
  bool pose_valid{false};
  /// True after a separately published controller-status sample has been
  /// correlated with the measured joints. Joint/pose validity is independent.
  bool status_valid{false};
  std::optional<std::int64_t> status_source_timestamp_ns;
  std::string controller_state{"unknown"};
  std::optional<std::int64_t> controller_status_code;
  std::vector<std::int64_t> controller_error_codes;
  std::array<ManipulatorJointDiagnostics, 6> joint_diagnostics{};
};

/// kUnknown persists until a valid hardware sample arrives.
enum class GripperState : std::uint8_t {
  kUnknown,
  kOpen,
  kClosed,
  kHolding,
};

/// Position: 0 closed, 100 open. Force is absent without calibration.
struct GripperTelemetry {
  GripperState state{GripperState::kUnknown};
  std::optional<double> force_n;
  std::optional<double> position_pct;
};

/// Domain of a source-provided event timestamp. Values from different domains
/// must never be subtracted or ordered against each other.
enum class SourceClockKind : std::uint8_t {
  kUnknown,
  kUnix,
  kSteadyBoot,
  kRos,
  kSimulation,
  kDevice,
};

inline const char* SourceClockName(SourceClockKind kind) noexcept {
  switch (kind) {
    case SourceClockKind::kUnknown:
      return "unknown";
    case SourceClockKind::kUnix:
      return "unix";
    case SourceClockKind::kSteadyBoot:
      return "steady_boot";
    case SourceClockKind::kRos:
      return "ros";
    case SourceClockKind::kSimulation:
      return "simulation";
    case SourceClockKind::kDevice:
      return "device";
  }
  return "unknown";
}

/// RGB and aligned depth share these pixel coordinates.
struct CameraIntrinsics {
  std::uint32_t width_px{0};
  std::uint32_t height_px{0};
  double fx{0.0};
  double fy{0.0};
  double ppx{0.0};
  double ppy{0.0};
  std::string distortion_model;
  std::array<double, 5> distortion_coefficients{};
};

/// Owned RGB sample with optional aligned depth.
struct CameraTelemetry {
  std::uint32_t width_px{0};
  std::uint32_t height_px{0};
  std::string frame_id;
  /// Driver publication time, not necessarily exposure time.
  std::optional<std::int64_t> source_timestamp_ns;
  SourceClockKind source_clock{SourceClockKind::kUnknown};
  std::string source_clock_id;
  std::vector<std::uint8_t> jpeg;
  /// Optional lossless 16-bit depth image aligned pixel-for-pixel to `jpeg`.
  std::vector<std::uint8_t> depth_png;
  /// Optional row-major aligned depth, avoiding PNG conversion in perception.
  std::vector<std::uint16_t> depth_u16;
  /// Multiplier from either depth representation's raw units to metres.
  double depth_scale{0.001};
  /// Calibration for the pixel coordinate system of the aligned depth image.
  std::optional<CameraIntrinsics> depth_intrinsics;
  /// Mount pose sampled with this frame; delayed perception must use this pose.
  std::optional<GimbalTelemetry> capture_gimbal;
  std::optional<std::int64_t> capture_gimbal_sampled_monotonic_ns;
  /// Immutable backend context; generic consumers carry it unchanged.
  std::shared_ptr<const void> opaque_context;
};

/// Owned signed 16-bit little-endian PCM; channels are interleaved.
struct MicrophoneTelemetry {
  std::uint32_t sample_rate_hz{16000};
  std::uint16_t channels{1};
  std::string frame_id;
  /// Driver publication time, not necessarily capture start time.
  std::optional<std::int64_t> source_timestamp_ns;
  SourceClockKind source_clock{SourceClockKind::kUnknown};
  std::string source_clock_id;
  std::vector<std::int16_t> pcm;
};

enum class SpeakerPlaybackState : std::uint8_t {
  kIdle,
  kBuffering,
  kPlaying,
  kDraining,
  kFault,
};

struct SpeakerTelemetry {
  SpeakerPlaybackState state{SpeakerPlaybackState::kIdle};
  std::optional<std::string> asset_id;
  std::optional<std::uint64_t> queued_duration_ms;
  std::optional<std::uint64_t> played_duration_ms;
  std::uint64_t underruns{0};
  std::optional<std::int64_t> source_timestamp_ns;
  SourceClockKind source_clock{SourceClockKind::kUnknown};
  std::string source_clock_id;
};

enum class ReleaseResult : std::uint8_t {
  kAccepted,  ///< backend operation succeeded at its documented boundary
  kRejected,  ///< backend refused; Dispatcher reports rejected.invalid_state
  kFailed,    ///< admitted command reached a terminal failure
  kCancelled,
  kTimedOut,
  kOutcomeUnknown,  ///< release may have happened; it must never be retried
};

/// Runtime hardware bridge owns and hot-plugs backend instances.
class Backend {
 public:
  virtual ~Backend() = default;

  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;

  /// Admission: report identity and declared feed rates. Called by the
  /// bridge before the backend is eligible to receive release.
  virtual BackendIdentity Identify() const = 0;
  virtual FeedDeclarations DeclareFeeds() const = 0;
  virtual BackendHealth ReadHealth() const = 0;

  /// Records successful bridge admission. Runtime bridge calls this exactly
  /// once after identity, configuration, and feed declarations validate.
  void MarkAdmitted() { admitted_.store(true); }

  /// Only manipulator backends accept.
  virtual ReleaseResult ExecuteMoveJ(const MoveJCommand& /*cmd*/) {
    return ReleaseResult::kRejected;
  }

  /// Only manipulator backends accept.
  virtual ReleaseResult ExecuteMoveL(const MoveLCommand& /*cmd*/) {
    return ReleaseResult::kRejected;
  }

  /// Release one typed world-object selection to a manipulator-side planner.
  virtual ReleaseResult ExecutePick(const PickCommand& /*cmd*/) { return ReleaseResult::kRejected; }

  /// Returns nullopt when unavailable or not an arm.
  virtual std::optional<ManipulatorTelemetry> ReadManipulatorTelemetry() const {
    return std::nullopt;
  }

  virtual std::optional<BaseTelemetry> ReadBaseTelemetry() const { return std::nullopt; }

  virtual std::optional<GimbalTelemetry> ReadGimbalTelemetry() const { return std::nullopt; }

  /// Returns nullopt until a valid gripper sample exists.
  virtual std::optional<GripperTelemetry> ReadGripperTelemetry() const { return std::nullopt; }

  /// Returns nullopt until a valid camera frame exists.
  virtual std::optional<CameraTelemetry> ReadCameraTelemetry() const { return std::nullopt; }

  /// Returns nullopt until a valid microphone sample exists.
  virtual std::optional<MicrophoneTelemetry> ReadMicrophoneTelemetry() const {
    return std::nullopt;
  }

  /// Returns nullopt until a valid speaker sample exists.
  virtual std::optional<SpeakerTelemetry> ReadSpeakerTelemetry() const { return std::nullopt; }

  /// Only base backends accept.
  virtual ReleaseResult ExecuteBase(const BaseCommand& /*cmd*/) { return ReleaseResult::kRejected; }

  /// Only navigation-capable base backends accept.
  virtual ReleaseResult ExecuteNavigateRelative(const BaseNavigateRelativeCommand& /*cmd*/) {
    return ReleaseResult::kRejected;
  }

  /// Only gimbal backends accept.
  virtual ReleaseResult ExecuteGimbal(const GimbalCommand& /*cmd*/) {
    return ReleaseResult::kRejected;
  }

  /// Only gripper-capable backends accept.
  virtual ReleaseResult ExecuteGripper(const GripperCommand& /*cmd*/) {
    return ReleaseResult::kRejected;
  }

  /// Only speaker backends accept.
  virtual ReleaseResult ExecuteSpeaker(const SpeakerCommand& /*cmd*/) {
    return ReleaseResult::kRejected;
  }

  /// Only posture-capable base backends accept.
  virtual ReleaseResult ExecutePosture(const BasePostureCommand& /*command*/) {
    return ReleaseResult::kRejected;
  }

  /// Stop active motion. Backends must return kAccepted only after the stop
  /// has been released to their driver; the default refuses an unverifiable stop.
  virtual ReleaseResult StopMotion() { return ReleaseResult::kRejected; }

  /// True only when StopMotion and Halt can safely preempt an in-flight call
  /// without the dispatcher's per-device lock.
  virtual bool CanStopWithoutDeviceLock() const { return false; }

  /// Cancel one correlated motion without affecting a later command. This may
  /// be called concurrently with the matching Execute* call after release has
  /// started. The default is suitable for synchronous/device-exclusive adapters.
  virtual ReleaseResult CancelCommand(const std::string& /*command_id*/) { return StopMotion(); }

  /// Operator-facing cause of the most recent rejected release.
  virtual std::string ReleaseFailureReason() const { return "backend refused the release"; }

  /// Bridge tick; never concurrent with a release.
  virtual void Tick(double /*dt_s*/) {}

  /// Safe-envelope primitives the backend must implement. Clamp: zero
  /// velocity, hold last position within torque limits. Halt: reach the same
  /// safe envelope within the backend-declared bounded time.
  virtual void Clamp() = 0;
  virtual void Halt() = 0;

 protected:
  Backend() = default;

  /// True only after the runtime bridge completed admission.
  bool IsAdmitted() const { return admitted_.load(); }

 private:
  std::atomic_bool admitted_{false};
};

}  // namespace eos::hardware
