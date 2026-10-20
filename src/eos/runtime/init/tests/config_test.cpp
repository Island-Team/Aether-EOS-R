#include "src/eos/runtime/init/config.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace eos::runtime::init {
namespace {

class ConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = std::filesystem::temp_directory_path() /
            ("eos-config-test-" + std::to_string(++sequence_) + ".toml");
  }
  void TearDown() override { std::filesystem::remove(path_); }
  void Write(const std::string& text) {
    std::ofstream output(path_);
    ASSERT_TRUE(output);
    output << text;
  }

  static inline unsigned sequence_{0};
  std::filesystem::path path_;
};

TEST_F(ConfigTest, LoadsCommittedConfiguration) {
  const Config config = LoadConfig(std::string(EOS_SOURCE_ETC) + "/eos.example.toml");
  EXPECT_EQ(config.host, "127.0.0.1");
  EXPECT_EQ(config.grpc_port, 16003);
  EXPECT_TRUE(config.grpc_uds_path.empty());
  EXPECT_EQ(config.grpc_uds_mode, 0600u);
  EXPECT_TRUE(config.workflow_journal_enabled);
  EXPECT_EQ(config.log.console_level, "off");
  EXPECT_EQ(config.log.file_level, "info");
  EXPECT_EQ(config.log.jsonl_level, "trace");
  EXPECT_FALSE(config.log.overwrite);
  EXPECT_TRUE(config.log.dedup);
  EXPECT_DOUBLE_EQ(config.log.dedup_window_s, 30.0);
  EXPECT_TRUE(config.log.save_attachments);
  EXPECT_EQ(config.log.rotation_size_bytes, 64ULL * 1024ULL * 1024ULL);
  EXPECT_EQ(config.log.rotation_files, 5U);
  EXPECT_TRUE(config.prelaunch.empty());
  ASSERT_EQ(config.mode.bindings.size(), 1u);
  EXPECT_EQ(config.mode.bindings[0].backend, "backends/demo.toml");
  EXPECT_EQ(config.mode.bindings[0].profile, "demo");
  EXPECT_TRUE(config.mode.expose_operator);
  ASSERT_EQ(config.models.size(), 2u);
  EXPECT_EQ(config.models[0].id, "arcface-register");
  EXPECT_EQ(config.models[1].id, "audio8");
  EXPECT_FALSE(config.perception_stream.vad);
  EXPECT_FALSE(config.perception_stream.human);
  EXPECT_FALSE(config.perception_stream.human_pose);
  EXPECT_FALSE(config.perception_stream.face);
}

TEST_F(ConfigTest, PerceptionStreamDefaultsFalseAndRequiresBooleans) {
  Write("[perception.stream]\nhuman=true\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  const Config config = LoadConfig(path_.string());
  EXPECT_FALSE(config.perception_stream.vad);
  EXPECT_TRUE(config.perception_stream.human);
  EXPECT_FALSE(config.perception_stream.human_pose);
  EXPECT_FALSE(config.perception_stream.face);

  Write("[perception.stream]\nvad='yes'\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);
  Write("[perception.stream]\nobjects=true\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);
}

TEST_F(ConfigTest, CameraPostureModelIsOptionalAndRequiresNonEmptyString) {
  Write("[perception.camera_pose]\nmodel_id='pose'\nrate_hz=10\n"
        "[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_TRUE(LoadConfig(path_.string()).camera_pose.posture_model_id.empty());

  Write("[perception.camera_pose]\nmodel_id='pose'\nposture_model_id='gemma'\nrate_hz=10\n"
        "[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_EQ(LoadConfig(path_.string()).camera_pose.posture_model_id, "gemma");

  Write("[perception.camera_pose]\nmodel_id='pose'\nposture_model_id=''\nrate_hz=10\n"
        "[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);
}

TEST_F(ConfigTest, ParsesDemandDrivenCameraObjectConfiguration) {
  Write(
      "[perception.camera_object]\n"
      "model_id='objects'\n"
      "camera_part_id='head'\n"
      "rate_hz=5\n"
      "[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  const Config config = LoadConfig(path_.string());
  EXPECT_EQ(config.camera_object.model_id, "objects");
  EXPECT_EQ(config.camera_object.camera_part_id, "head");
  EXPECT_EQ(config.camera_object.rate_hz, 5U);
}

TEST_F(ConfigTest, ParsesBoundedArmGraspConfiguration) {
  Write("[perception.arm_grasp]\n"
        "observe_joint_positions_rad=[0.0,-0.1,1.2,0.0,1.7,0.0]\n"
        "wrist_camera_device_id='cam-0'\n"
        "workspace_min_m=[-0.7,-0.3,-0.55]\n"
        "workspace_max_m=[-0.1,0.3,0.4]\n"
        "[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  const Config config = LoadConfig(path_.string());
  EXPECT_EQ(config.arm_grasp.observe_joint_positions_rad.size(), 6u);
  EXPECT_EQ(config.arm_grasp.wrist_camera_device_id, "cam-0");
  EXPECT_EQ(config.arm_grasp.workspace_min_m.front(), -0.7);

  Write("[perception.arm_grasp]\n"
        "observe_joint_positions_rad=[0.0,-0.1,1.2,0.0,1.7,0.0]\n"
        "wrist_camera_device_id='cam-0'\n"
        "workspace_min_m=[-0.1,-0.3,-0.55]\n"
        "workspace_max_m=[-0.7,0.3,0.4]\n"
        "[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);
}

TEST_F(ConfigTest, LoadsTestConfiguration) {
  const Config config = LoadConfig(std::string(EOS_SOURCE_ETC) + "/test/eos.test.toml");
  EXPECT_EQ(config.grpc_port, 16003);
  ASSERT_EQ(config.mode.bindings.size(), 1u);
  EXPECT_EQ(config.mode.bindings.front().profile, "demo");
  EXPECT_FALSE(config.mode.expose_operator);
  ASSERT_EQ(config.models.size(), 4u);
}

TEST_F(ConfigTest, ParsesPrelaunchCommandsInDeclarationOrder) {
  Write(
      "[launch]\n"
      "[[launch.prelaunch]]\n"
      "command = ['/bin/true']\n"
      "[[launch.prelaunch]]\n"
      "command = ['/bin/sh', '-c', 'exit 0']\n"
      "[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  const Config config = LoadConfig(path_.string());
  ASSERT_EQ(config.prelaunch.size(), 2u);
  EXPECT_EQ(config.prelaunch[0].arguments, (std::vector<std::string>{"/bin/true"}));
  EXPECT_EQ(config.prelaunch[1].arguments, (std::vector<std::string>{"/bin/sh", "-c", "exit 0"}));
}

TEST_F(ConfigTest, WorkflowJournalIsOptionalAndStrictlyTyped) {
  Write("[workflow]\njournal_enabled=true\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_TRUE(LoadConfig(path_.string()).workflow_journal_enabled);

  Write("[workflow]\njournal_enabled='yes'\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write(
      "[workflow]\npath='/tmp/"
      "client-selected'\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);
}

TEST_F(ConfigTest, ParsesOptionalGrpcUnixSocket) {
  Write(
      "[gateway]\n"
      "grpc_uds_path='/run/island/eos.sock'\n"
      "grpc_uds_mode=0o660\n"
      "[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  const Config config = LoadConfig(path_.string());
  EXPECT_EQ(config.grpc_uds_path, "/run/island/eos.sock");
  EXPECT_EQ(config.grpc_uds_mode, 0660u);

  Write("[gateway]\ngrpc_uds_path='relative.sock'\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write("[gateway]\ngrpc_uds_mode=0o1000\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write("[gateway]\ngrpc_uds_mode=0o600\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);
}

TEST_F(ConfigTest, ParsesMultiplePrelaunchShellServicesInDeclarationOrder) {
  Write(
      "[launch]\n[[launch.prelaunch]]\nshell = 'exec ./models/vision.sh'\n"
      "[[launch.prelaunch]]\nshell = 'exec ./models/audio.sh'\n"
      "[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  const Config config = LoadConfig(path_.string());
  ASSERT_EQ(config.prelaunch.size(), 2u);
  EXPECT_EQ(config.prelaunch[0].shell, "exec ./models/vision.sh");
  EXPECT_TRUE(config.prelaunch[0].arguments.empty());
  EXPECT_EQ(config.prelaunch[1].shell, "exec ./models/audio.sh");
  EXPECT_TRUE(config.prelaunch[1].arguments.empty());
}

TEST_F(ConfigTest, RejectsUnknownRootAndSectionKeys) {
  Write("mystery = true\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write("[gateway]\nlisten = 'x'\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write("[mode]\nextra=true\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write("[perception]\nextra=true\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write(
      "[perception.camera_pose]\nmodel_id='pose'\nrate_hz=0\n[mode]\n[[mode.binding]]\nbackend='x'"
      "\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write(
      "[perception.camera_face]\nmodel_id=''\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'"
      "\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write(
      "[perception.camera_object]\nmodel_id=''\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'"
      "\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write(
      "[perception.camera_object]\nmodel_id='object'\nrate_hz=0\n"
      "[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write(
      "[perception.camera_face]\nmodel_id='face'\nregistration_model_id='face-register'\n"
      "[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'"
      "\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write(
      "[perception.microphone_vad]\nmodel_id='vad'\nwindow_ms=0\n[mode]\n[[mode.binding]]\nbackend="
      "'x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write(
      "[perception.microphone_vad]\nmodel_id='vad'\nwindow_ms=500\nsave_audio=false\noverwrite_"
      "audio=true\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write(
      "[perception.microphone_vad]\nmodel_id='vad'\nasr_model_id=''\nwindow_ms=500\n"
      "[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write("[launch]\nunknown=true\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);
}

TEST_F(ConfigTest, RejectsWrongTypesAndReservedPorts) {
  Write("[gateway]\nhost=1\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write(
      "[gateway]\ngrpc_port=16004\n[mode]\n[[mode.binding]]\nbackend='x'\n"
      "profile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write("[gateway]\njson_port=16005\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write("[log]\ndedup='yes'\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write("[log]\ndedup_window_s=0.0\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write("[log]\nrotation_size_bytes=0\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write("[log]\nrotation_files=101\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);

  Write("[launch]\nprelaunch = [{}]\n[mode]\n[[mode.binding]]\nbackend='x'\nprofile='x'\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);
}

TEST_F(ConfigTest, NamesMissingAndMalformedFiles) {
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);
  Write("[[broken\n");
  EXPECT_THROW(LoadConfig(path_.string()), std::runtime_error);
}

}  // namespace
}  // namespace eos::runtime::init
