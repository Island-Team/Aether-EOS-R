#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/eos/lib/log.h"
#include "src/eos/runtime/hardware/mode.h"
#include "src/eos/runtime/init/prelaunch.h"
#include "src/eos/runtime/models/runtime.h"

namespace eos::runtime::init {

struct Config {
  struct PerceptionStreamConfig {
    bool vad{false};
    bool human{false};
    bool human_pose{false};
    bool face{false};
  };
  struct CameraPoseConfig {
    std::string model_id;
    std::string detection_model_id;
    std::string posture_model_id;
    std::string camera_part_id{"default"};
    std::string gimbal_part_id{"default"};
    std::uint32_t rate_hz{0};
  };
  struct CameraFaceConfig {
    std::string model_id;
  };
  struct CameraObjectConfig {
    std::string model_id;
    std::string camera_part_id{"default"};
    std::uint32_t rate_hz{0};
  };
  struct ArmGraspConfig {
    std::vector<double> observe_joint_positions_rad;
    std::string wrist_camera_device_id;
    std::vector<double> workspace_min_m;
    std::vector<double> workspace_max_m;
  };
  struct MicrophoneVadConfig {
    std::string model_id;
    std::string asr_model_id{"asr"};
    std::uint32_t window_ms{0};
    bool save_audio{false};
    bool overwrite_audio{false};
  };

  std::string host{"127.0.0.1"};
  std::uint16_t grpc_port{16003};
  std::string grpc_uds_path;
  std::uint32_t grpc_uds_mode{0600};
  bool workflow_journal_enabled{false};
  lib::LogConfig log;
  hardware_runtime::ModeConfig mode;
  std::vector<models::ModelConfig> models;
  std::vector<PrelaunchCommand> prelaunch;
  ManagedServiceConfig adapter;
  PerceptionStreamConfig perception_stream;
  CameraPoseConfig camera_pose;
  CameraFaceConfig camera_face;
  CameraObjectConfig camera_object;
  ArmGraspConfig arm_grasp;
  MicrophoneVadConfig microphone_vad;
};

/// Loads and strictly validates the complete process configuration.
Config LoadConfig(const std::string& path);

}  // namespace eos::runtime::init
