#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eos::ext::simulator::ipc {

struct ArmState { double ee_position[3]{}; double ee_orientation_quat[4]{}; double ee_linear_velocity[3]{}; double joint_positions_rad[6]{}; double joint_velocities[6]{}; bool contact{false}; };
struct BaseState { double x{0}, y{0}, yaw{0}, vx{0}, vy{0}, yaw_rate{0}; std::uint8_t posture{0}; };
struct GimbalState { double pan_rad{0}, tilt_rad{0}, pan_rate{0}, tilt_rate{0}; };
struct MicrophoneBlock { std::uint32_t sample_rate_hz{16000}; std::uint16_t channels{1}; std::int16_t samples[64]{}; };
enum class GripperJaw : std::uint8_t { kOpen, kClosed, kHolding };
struct GripperState { GripperJaw state{GripperJaw::kOpen}; std::string object_held_id; double force_n{5}; double position_pct{100}; };
struct SpeakerState { bool enabled{true}, device_present{true}, playing{false}; double remaining_s{0}; };
struct StateSnapshot { std::uint64_t state_sequence{0}; BaseState base; GimbalState gimbal; std::uint64_t scene_revision{0}; std::optional<std::string> selected_line_id; std::optional<std::string> selected_source_id; };
struct CameraSourceFrame { std::uint32_t width_px{0}, height_px{0}; std::int64_t source_timestamp_ns{0}; std::vector<std::uint8_t> jpeg; StateSnapshot state; };

/// Bounded, thread-safe client for the separately launched simulator. Each
/// call creates its own deadline, so a missing service can never stall eosd.
class SimulatorClient {
 public:
  explicit SimulatorClient(std::filesystem::path socket_path,
                           std::chrono::milliseconds deadline = std::chrono::milliseconds(250));
  ~SimulatorClient();
  SimulatorClient(const SimulatorClient&) = delete;
  SimulatorClient& operator=(const SimulatorClient&) = delete;

  const std::filesystem::path& socket_path() const;
  bool Available(const std::string& device_id = {}) const;

  bool SetCartesian(const std::string&, const double position[3], const double orientation[4]);
  bool SetJointPositions(const std::string&, const std::vector<double>&);
  bool StopArm(const std::string&);
  std::optional<ArmState> ReadArm(const std::string&) const;
  bool DriveBase(const std::string&, double, double, double);
  bool NavigateBaseRelative(const std::string&, double, double, double, double, double);
  bool SetBasePosture(const std::string&, std::uint8_t);
  bool StopBase(const std::string&);
  std::optional<BaseState> ReadBase(const std::string&) const;
  bool PointGimbal(const std::string&, double, double, double, double);
  bool StopGimbal(const std::string&);
  std::optional<GimbalState> ReadGimbal(const std::string&) const;
  bool MoveGripper(const std::string&, bool, double);
  bool ArmGrasp(const std::string&, const std::string&);
  bool StopGripper(const std::string&);
  std::optional<GripperState> ReadGripper(const std::string&) const;
  bool PlayAudio(const std::string&, std::size_t);
  bool StopSpeaker(const std::string&);
  std::optional<SpeakerState> ReadSpeaker(const std::string&) const;
  std::optional<MicrophoneBlock> ReadMicrophone(const std::string&) const;
  std::optional<CameraSourceFrame> ReadCameraSource(const std::string&) const;
  bool Halt(const std::string&);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace eos::ext::simulator::ipc
