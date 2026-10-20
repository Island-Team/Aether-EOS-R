/// Binding parser tests: the canonical profiled shape (every file is
/// [profile.<name>] + [[device]] entries), against both inline fixtures and
/// the committed etc/eos/backends files, plus the fail-loudly paths (unknown
/// class/backend, unknown keys, missing feed classes, primitive admission,
/// ros2 endpoint groups).
#include "binding.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using eos::runtime::hardware_runtime::Binding;
using eos::runtime::hardware_runtime::LoadBinding;
using eos::runtime::hardware_runtime::ParseBinding;
using eos::runtime::hardware_runtime::PrimitiveSpecOf;

TEST(BindingTest, EveryPrimitiveKindHasDescription) {
  const char* const names[] = {"base.velocity",  "base.navigate", "base.posture",  "gimbal.motion",
                               "gripper.motion", "speaker.play",  "speaker.speak", "face.register",
                               "arm.init",       "arm.movej",     "arm.movel",     "arm.pick",
                               "motion.stop",    "robot.stop",    "safety.estop"};
  for (const char* name : names) {
    const auto* spec = PrimitiveSpecOf(name, "1.0.0");
    ASSERT_NE(spec, nullptr) << name;
    EXPECT_FALSE(std::string(spec->description).empty()) << name;
  }
}

/// Parses one [profile.test] body (its [[profile.test.device]] entries,
/// fully-qualified TOML headers) and returns the binding.
Binding ParseDevices(const std::string& devices, const std::string& path) {
  return ParseBinding(devices, path, "test");
}

/// Parses and returns the error message of a throwing parse.
std::string ParseErrorOf(const std::string& devices, const std::string& path) {
  try {
    ParseDevices(devices, path);
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return "";
}

/// One device body for DeviceEntry: class, backend table, and active
/// demo feed rates, plus the given extra `body`. `body` lines must use
/// fully-qualified [profile.test.device.*] headers — TOML headers are
/// absolute; there is no lexical nesting off [profile.test].
std::string DeviceEntry(const std::string& device_class, const std::string& backend_type,
                        const std::string& body = "") {
  struct Feed {
    const char* name;
    double rate;
  };
  Feed prop{"proprioception", 0.0};
  Feed sensor{"sensor", 0.0};
  if (device_class == "base") prop = {"proprioception", 100.0};
  if (device_class == "gimbal") prop = {"proprioception", 200.0};
  if (device_class == "arm") prop = {"proprioception", 500.0};
  if (device_class == "cam") sensor = {"sensor", 30.0};
  if (device_class == "mic") sensor = {"sensor", 250.0};
  if (device_class == "gripper") prop = {"proprioception", 50.0};
  if (device_class == "speaker") prop = {"proprioception", 10.0};
  std::string text = "[[profile.test.device]]\nclass = \"" + device_class + "\"\n";
  if (!body.empty()) {
    text += "\n" + body + "\n";
  }
  text += "[profile.test.device.backend]\ntype = \"" + backend_type + "\"\n";
  if (prop.rate > 0.0) {
    text += "\n[profile.test.device.feeds." + std::string(prop.name) +
            "]\nmax_rate_hz = " + std::to_string(prop.rate) + "\n";
  }
  if (sensor.rate > 0.0) {
    text += "\n[profile.test.device.feeds." + std::string(sensor.name) +
            "]\nmax_rate_hz = " + std::to_string(sensor.rate) + "\n";
  }
  text += "\n[profile.test.device.feeds.health]\nmax_rate_hz = 10.0\n";
  return text;
}

/// One [[profile.test.device.primitive]] block.
std::string Primitive(const std::string& name) {
  std::string limits;
  std::string reference;
  if (name == "base.velocity") {
    limits =
        "\nlimits = { linear_x_m_s = { minimum = -2.0, maximum = 2.0 }, "
        "linear_y_m_s = { minimum = -1.0, maximum = 1.0 }, "
        "angular_z_rad_s = { minimum = -3.0, maximum = 3.0 }, "
        "valid_for_ms = { minimum = 1.0, maximum = 1000.0 } }";
  } else if (name == "base.navigate") {
    reference = "\nreferences = [\"relative\"]";
    limits =
        "\nlimits = { x_m = { minimum = -10.0, maximum = 10.0 }, "
        "y_m = { minimum = -10.0, maximum = 10.0 }, "
        "yaw_rad = { minimum = -6.283185307179586, maximum = 6.283185307179586 } }";
  } else if (name == "gimbal.motion") {
    reference = "\nreferences = [\"relative\", \"absolute\"]\nabsolute_frame = \"base_link\"";
    limits =
        "\nlimits = { pan_rad = { minimum = -3.14, maximum = 3.14 }, "
        "tilt_rad = { minimum = -1.57, maximum = 1.57 }, "
        "speed_rad_s = { minimum = 0.0, minimum_exclusive = true } }";
  } else if (name == "arm.movej") {
    reference = "\nreferences = [\"relative\", \"absolute\"]";
    limits =
        "\nlimits = { joint_positions_rad = { minimum = -3.14, maximum = 3.14 }, "
        "speed_rad_s = { minimum = 0.0, minimum_exclusive = true } }";
  } else if (name == "arm.movel") {
    reference = "\nreferences = [\"relative\", \"absolute\"]\nabsolute_frame = \"base_link\"";
    limits = "\nlimits = { speed_m_s = { minimum = 0.0, minimum_exclusive = true } }";
  } else if (name == "gripper.motion") {
    limits =
        "\nlimits = { force_n = { minimum = 0.0, maximum = 10.0, "
        "minimum_exclusive = true } }";
  }
  return "[[profile.test.device.primitive]]\nname = \"" + name +
         "\"\nversion = \"1.0.0\"\ndescription = \"test declaration\"" + reference + limits;
}

TEST(BindingTest, SingleDeviceProfileParses) {
  const std::string identity =
      "part_id = \"default\"\ndescription = \"left drive\"\ntags = [\"locomotion\"]";
  const Binding binding = ParseDevices(DeviceEntry("base", "demo",
                                                   identity + "\n\n" + Primitive("base.velocity") +
                                                       "\n\n" + Primitive("base.posture")),
                                       "inline.toml");
  ASSERT_EQ(binding.devices.size(), 1u);
  const auto& device = binding.devices[0];
  EXPECT_EQ(device.backend_type, "demo");
  EXPECT_TRUE(device.device_id.empty());
  EXPECT_EQ(device.part.kind, "base");
  EXPECT_EQ(device.part.part_id, "default");
  ASSERT_TRUE(device.part.description.has_value());
  EXPECT_EQ(*device.part.description, "left drive");
  ASSERT_EQ(device.part.primitives.size(), 2u);
  EXPECT_EQ(device.part.primitives[0].name, "base.velocity");
  EXPECT_EQ(device.part.primitives[1].name, "base.posture");
  EXPECT_DOUBLE_EQ(device.envelope.proprioception.max_rate_hz, 100.0);
  EXPECT_DOUBLE_EQ(device.envelope.geometric.max_rate_hz, 0.0);
  EXPECT_DOUBLE_EQ(device.envelope.sensor.max_rate_hz, 0.0);
  EXPECT_DOUBLE_EQ(device.envelope.health.max_rate_hz, 10.0);
}

TEST(BindingTest, ArmInitRequiresOneConfiguredPositionPerJoint) {
  const std::string missing =
      ParseErrorOf(DeviceEntry("arm", "demo", Primitive("arm.init")), "arm-init-missing.toml");
  EXPECT_NE(missing.find("initial_joint_positions_rad"), std::string::npos);

  const std::string mismatch =
      ParseErrorOf(DeviceEntry("arm", "demo",
                               "joint_names = [\"shoulder\", \"elbow\"]\n\n"
                               "[profile.test.device.part]\n"
                               "initial_joint_positions_rad = [0.1]\n\n" +
                                   Primitive("arm.init")),
                   "arm-init-mismatch.toml");
  EXPECT_NE(mismatch.find("one value for every configured joint"), std::string::npos);
}

TEST(BindingTest, ProfileIsRequired) {
  std::string error;
  try {
    ParseBinding(DeviceEntry("base", "demo"), "inline.toml", "");
  } catch (const std::runtime_error& e) {
    error = e.what();
  }
  EXPECT_NE(error.find("a profile is required"), std::string::npos);

  // The legacy single-device shape no longer parses: it is not a profiled
  // file at all.
  const std::string legacy =
      ParseErrorOf("backend = \"demo-base\"\ndevice_id = \"x\"\n", "legacy.toml");
  EXPECT_NE(legacy.find("no '[profile.<name>]' tables"), std::string::npos);
}

TEST(BindingTest, UnknownProfileFails) {
  std::string error;
  try {
    LoadBinding(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "ghost");
  } catch (const std::runtime_error& e) {
    error = e.what();
  }
  EXPECT_NE(error.find("profile 'ghost' not found"), std::string::npos);
}

TEST(BindingTest, RobotIdentityParses) {
  const Binding binding = ParseDevices(
      "[profile.test.robot]\nrobot_id = \"robot-a\"\n"
      "robot_name = \"alpha\"\nrobot_desc = \"test robot\"\n"
      "model = \"island-x\"\nvendor = \"example\"\n" +
          DeviceEntry("base", "demo"),
      "identity.toml");
  EXPECT_EQ(binding.robot.id, "robot-a");
  EXPECT_EQ(binding.robot.name, "alpha");
  ASSERT_TRUE(binding.robot.model.has_value());
  EXPECT_EQ(*binding.robot.model, "island-x");
  ASSERT_TRUE(binding.robot.vendor.has_value());
  EXPECT_EQ(*binding.robot.vendor, "example");
  ASSERT_TRUE(binding.robot.description.has_value());
  EXPECT_EQ(*binding.robot.description, "test robot");
}

TEST(BindingTest, ConfiguredRuntimeAndDeviceIdsAreRejected) {
  const std::string device = ParseErrorOf(
      "[[profile.test.device]]\nclass = \"base\"\nid = \"base-9\"\n", "device-id.toml");
  EXPECT_NE(device.find("obsolete key 'id'"), std::string::npos);
  EXPECT_NE(device.find("<class>-<index>"), std::string::npos);

  const std::string robot = ParseErrorOf(
      "[profile.test.robot]\nid = \"robot-a\"\n" + DeviceEntry("base", "demo"), "robot-id.toml");
  EXPECT_NE(robot.find("rename it to 'robot_id'"), std::string::npos);
}

/// A valid ros2 endpoint set covering the arm primitive surface.
const char* kRos2Endpoints = R"([profile.test.device.backend.ros2]
domain_id = 0
namespace = "/eos"

[profile.test.device.backend.ros2.joint_states]
kind = "topic"
name = "/joint_states"
message = "sensor_msgs/JointState"

[profile.test.device.backend.ros2.pose_query]
kind = "topic"
name = "/pose"
message = "geometry_msgs/PoseStamped"

[profile.test.device.backend.ros2.stop]
kind = "topic"
name = "/stop"
message = "std_msgs/Empty"

[profile.test.device.backend.ros2.estop]
kind = "topic"
name = "/estop"
message = "std_msgs/Empty"

[profile.test.device.backend.ros2.movel]
kind = "topic"
name = "/goal"
message = "std_msgs/Float64MultiArray"

[profile.test.device.backend.ros2.movej]
kind = "topic"
name = "/traj"
message = "std_msgs/Float64MultiArray"

[profile.test.device.backend.ros2.pick]
kind = "topic"
name = "/pick"
message = "std_msgs/String"

[profile.test.device.backend.ros2.command_result]
kind = "topic"
name = "/command/result"
message = "std_msgs/String"

[profile.test.device.backend.ros2.command_cancel]
kind = "topic"
name = "/command/cancel"
message = "std_msgs/String")";

const char* kRos2GripperEndpoints = R"([profile.test.device.backend.ros2]
domain_id = 0
namespace = "/eos"

[profile.test.device.backend.ros2.gripper_command]
kind = "topic"
name = "/eos/gripper/command"
message = "std_msgs/Float64MultiArray"

[profile.test.device.backend.ros2.gripper_state]
kind = "topic"
name = "/eos/gripper/state"
message = "std_msgs/Float64MultiArray"

[profile.test.device.backend.ros2.command_result]
kind = "topic"
name = "/eos/gripper/command_result"
message = "std_msgs/String"

[profile.test.device.backend.ros2.command_cancel]
kind = "topic"
name = "/eos/gripper/command_cancel"
message = "std_msgs/String"

[profile.test.device.backend.ros2.estop]
kind = "topic"
name = "/eos/gripper/estop"
message = "std_msgs/Empty")";

const char* kRos2CameraEndpoints = R"([profile.test.device.backend.ros2]
domain_id = 0
namespace = "/eos"
max_image_bytes = 4194304
depth_scale = 0.001

[profile.test.device.backend.ros2.image]
kind = "topic"
name = "/vision/dep/raw/compressed"
message = "sensor_msgs/CompressedImage"

[profile.test.device.backend.ros2.depth_image]
kind = "topic"
name = "/vision/dep/depth"
message = "sensor_msgs/Image")";

const char* kAlsaMicConfig = R"([profile.test.device.backend.alsa]
alsa_device = "test-device"
sample_rate_hz = 16000
channels = 1
window_frames = 640
)";

const char* kV4l2CamConfig = R"([profile.test.device.backend.v4l2]
video_device = "/dev/video2"
width_px = 1280
height_px = 720
fps = 15
read_timeout_ms = 3000
)";

const char* kAplaySpeakerConfig = R"([profile.test.device.backend.aplay]
alsa_device = "test-device"
max_stream_bytes = 1048576)";

TEST(BindingTest, JointNamesAreChannelOrderOnly) {
  // The binding carries joint NAMES (channel order for the graph), never
  // ranges/types/limits — that is URDF data.
  const Binding binding =
      ParseDevices(DeviceEntry("arm", "ros2",
                               "joint_names = [\"j_a\", \"j_b\"]\n\n" +
                                   std::string(kRos2Endpoints) + "\n\n" + Primitive("arm.movej")),
                   "names.toml");
  const auto& part = binding.devices[0].part;
  ASSERT_EQ(part.joint_names.size(), 2u);
  EXPECT_EQ(part.joint_names[0], "j_a");

  // A key list is for jointed classes only, and 1..6 entries.
  EXPECT_NE(ParseErrorOf(DeviceEntry("base", "demo", "joint_names = [\"j\"]"), "cross.toml")
                .find("only valid for class 'arm' or 'gimbal'"),
            std::string::npos);
  EXPECT_NE(ParseErrorOf(DeviceEntry("arm", "demo", "joint_names = []"), "empty.toml")
                .find("1..6 joint names"),
            std::string::npos);
}

TEST(BindingTest, RejectsDuplicatePartIdsWithinOneDeviceClass) {
  const std::string error = ParseErrorOf(DeviceEntry("cam", "demo") + DeviceEntry("cam", "demo"),
                                         "duplicate-camera-part.toml");
  EXPECT_NE(error.find("duplicate cam part_id 'default'"), std::string::npos);
}

TEST(BindingTest, RemovedWorkspaceDefaultsAreRejected) {
  EXPECT_NE(
      ParseErrorOf(DeviceEntry("arm", "demo", "[profile.test.device.workspace]\nplane_z_m = 0.3\n"),
                   "workspace.toml")
          .find("unknown key 'workspace'"),
      std::string::npos);
  // Nor do 3-D mounts.
  EXPECT_NE(
      ParseErrorOf(DeviceEntry("arm", "demo", "[profile.test.device.mount]\non = \"robot\"\n"),
                   "mount.toml")
          .find("unknown key 'mount'"),
      std::string::npos);

  EXPECT_NE(
      ParseErrorOf(DeviceEntry("arm", "demo", "[profile.test.device.motion]\nspeed_scale = 10\n"),
                   "motion.toml")
          .find("unknown key 'motion'"),
      std::string::npos);
}

TEST(BindingTest, PrimitiveAdmissionChecks) {
  const std::string missing_description = ParseErrorOf(
      DeviceEntry(
          "base", "demo",
          "[[profile.test.device.primitive]]\nname = \"base.velocity\"\nversion = \"1.0.0\""),
      "description.toml");
  EXPECT_NE(missing_description.find("missing required key 'description'"), std::string::npos);
  const Binding empty_description = ParseDevices(
      DeviceEntry(
          "base", "demo",
          "[[profile.test.device.primitive]]\nname = \"base.velocity\"\nversion = \"1.0.0\"\n"
          "description = \"\"\n"
          "limits = { linear_x_m_s = { minimum = -1.0, maximum = 1.0 }, "
          "linear_y_m_s = { minimum = -1.0, maximum = 1.0 }, "
          "angular_z_rad_s = { minimum = -1.0, maximum = 1.0 }, "
          "valid_for_ms = { minimum = 1.0, maximum = 1000.0 } }"),
      "empty-description.toml");
  EXPECT_EQ(empty_description.devices[0].part.primitives[0].description, "");

  const std::string missing_version = ParseErrorOf(
      DeviceEntry(
          "base", "demo",
          "[[profile.test.device.primitive]]\nname = \"base.velocity\"\ndescription = \"\""),
      "version.toml");
  EXPECT_NE(missing_version.find("missing required key 'version'"), std::string::npos);
  const std::string wrong_version = ParseErrorOf(
      DeviceEntry(
          "base", "demo",
          "[[profile.test.device.primitive]]\nname = \"base.velocity\"\nversion = \"2.0.0\"\n"
          "description = \"\""),
      "wrong-version.toml");
  EXPECT_NE(wrong_version.find("unknown primitive 'base.velocity' version '2.0.0'"),
            std::string::npos);

  const std::string unknown_primitive =
      ParseErrorOf(DeviceEntry("arm", "demo", Primitive("arm.wave")), "unknown.toml");
  EXPECT_NE(unknown_primitive.find("unknown primitive 'arm.wave'"), std::string::npos);
  const std::string privileged =
      ParseErrorOf(DeviceEntry("arm", "demo", Primitive("safety.estop")), "priv.toml");
  EXPECT_NE(privileged.find("privileged primitive 'safety.estop' is implicit"), std::string::npos);
  const std::string cross_class =
      ParseErrorOf(DeviceEntry("base", "demo", Primitive("arm.movej")), "cross.toml");
  EXPECT_NE(cross_class.find("belongs to class 'arm'"), std::string::npos);
  // A removed transport fails before the command is admitted.
  const std::string wrong_type =
      ParseErrorOf(DeviceEntry("arm", "sim", Primitive("arm.movej")), "types.toml");
  EXPECT_NE(wrong_type.find("unknown backend type 'sim'"), std::string::npos);
  const std::string wrong_tier = ParseErrorOf(
      DeviceEntry("arm", "demo", Primitive("arm.movej") + "\ntier = \"standard\""), "tier.toml");
  EXPECT_NE(wrong_tier.find("catalog fixes the other tier"), std::string::npos);
}

TEST(BindingTest, AdvancedPrimitiveRequiresExactModelBinding) {
  const std::string missing = ParseErrorOf(
      DeviceEntry("speaker", "demo", Primitive("speaker.speak")), "advanced-missing-model.toml");
  EXPECT_NE(missing.find("advanced primitive 'speaker.speak' requires a non-empty model_id"),
            std::string::npos);

  const Binding binding = ParseDevices(
      DeviceEntry("speaker", "demo", Primitive("speaker.speak") + "\nmodel_id = \"tts-main\""),
      "advanced.toml");
  ASSERT_EQ(binding.devices[0].part.primitives.size(), 1u);
  EXPECT_EQ(binding.devices[0].part.primitives[0].model_id, "tts-main");

  const std::string direct = ParseErrorOf(
      DeviceEntry("speaker", "demo", Primitive("speaker.play") + "\nmodel_id = \"tts-main\""),
      "direct-model.toml");
  EXPECT_NE(direct.find("model_id' is only valid for advanced primitives"), std::string::npos);
  const std::string empty_direct =
      ParseErrorOf(DeviceEntry("speaker", "demo", Primitive("speaker.play") + "\nmodel_id = \"\""),
                   "direct-empty-model.toml");
  EXPECT_NE(empty_direct.find("model_id' is only valid for advanced primitives"),
            std::string::npos);
}

TEST(BindingTest, MotionReferencesAreExplicitAndValidated) {
  const Binding relative = ParseDevices(DeviceEntry("base", "demo", Primitive("base.navigate")),
                                        "relative-reference.toml");
  EXPECT_EQ(relative.devices[0].part.primitives[0].references,
            std::vector<std::string>({"relative"}));
  EXPECT_FALSE(relative.devices[0].part.primitives[0].absolute_frame.has_value());

  const std::string missing =
      ParseErrorOf(DeviceEntry("base", "demo",
                               "[[profile.test.device.primitive]]\nname = \"base.navigate\"\n"
                               "version = \"1.0.0\"\ndescription = \"missing references\"\n"
                               "limits = { x_m = { minimum = -1.0, maximum = 1.0 }, "
                               "y_m = { minimum = -1.0, maximum = 1.0 }, "
                               "yaw_rad = { minimum = -1.0, maximum = 1.0 } }"),
                   "missing-references.toml");
  EXPECT_NE(missing.find("requires key 'references'"), std::string::npos);

  const std::string missing_frame =
      ParseErrorOf(DeviceEntry("base", "demo",
                               "[[profile.test.device.primitive]]\nname = \"base.navigate\"\n"
                               "version = \"1.0.0\"\ndescription = \"absolute without frame\"\n"
                               "references = [\"absolute\"]\n"
                               "limits = { x_m = { minimum = -1.0, maximum = 1.0 }, "
                               "y_m = { minimum = -1.0, maximum = 1.0 }, "
                               "yaw_rad = { minimum = -1.0, maximum = 1.0 } }"),
                   "missing-frame.toml");
  EXPECT_NE(missing_frame.find("absolute reference requires key 'absolute_frame'"),
            std::string::npos);

  const std::string duplicate = ParseErrorOf(
      DeviceEntry("arm", "demo",
                  "[[profile.test.device.primitive]]\nname = \"arm.movej\"\n"
                  "version = \"1.0.0\"\ndescription = \"duplicate references\"\n"
                  "references = [\"relative\", \"relative\"]\n"
                  "limits = { joint_positions_rad = { minimum = -3.14, maximum = 3.14 } }"),
      "duplicate-reference.toml");
  EXPECT_NE(duplicate.find("duplicate value 'relative'"), std::string::npos);
}

TEST(BindingTest, PrimitiveLimitsAreResolvedAndValidated) {
  const Binding binding =
      ParseDevices(DeviceEntry("base", "demo", Primitive("base.velocity")), "limits.toml");
  const auto& limits = binding.devices[0].part.primitives[0].limits;
  ASSERT_EQ(limits.size(), 4u);
  ASSERT_TRUE(limits.at("linear_x_m_s").minimum.has_value());
  ASSERT_TRUE(limits.at("linear_x_m_s").maximum.has_value());
  EXPECT_DOUBLE_EQ(*limits.at("linear_x_m_s").minimum, -2.0);
  EXPECT_DOUBLE_EQ(*limits.at("linear_x_m_s").maximum, 2.0);
  EXPECT_TRUE(limits.at("linear_x_m_s").Contains(2.0));
  EXPECT_FALSE(limits.at("linear_x_m_s").Contains(2.1));

  const std::string missing =
      ParseErrorOf(DeviceEntry("base", "demo",
                               "[[profile.test.device.primitive]]\nname = \"base.velocity\"\n"
                               "version = \"1.0.0\"\n"
                               "description = \"missing one range\"\n"
                               "limits = { linear_x_m_s = { minimum = -1.0, maximum = 1.0 }, "
                               "linear_y_m_s = { minimum = -1.0, maximum = 1.0 }, "
                               "valid_for_ms = { minimum = 1.0, maximum = 1000.0 } }"),
                   "missing-limit.toml");
  EXPECT_NE(missing.find("missing required limit 'angular_z_rad_s'"), std::string::npos);

  const std::string inverted = ParseErrorOf(
      DeviceEntry("arm", "demo",
                  "[[profile.test.device.primitive]]\nname = \"arm.movej\"\n"
                  "version = \"1.0.0\"\n"
                  "description = \"bad range\"\n"
                  "references = [\"absolute\"]\n"
                  "limits = { joint_positions_rad = { minimum = 1.0, maximum = -1.0 } }"),
      "inverted-limit.toml");
  EXPECT_NE(inverted.find("bounds do not form a non-empty finite range"), std::string::npos);
}

TEST(BindingTest, OptionalNumericArgumentsMayBeUnsupported) {
  const Binding gripper =
      ParseDevices(DeviceEntry("gripper", "demo",
                               "[[profile.test.device.primitive]]\nname = \"gripper.motion\"\n"
                               "version = \"1.0.0\"\n"
                               "description = \"default force only\""),
                   "default-force.toml");
  EXPECT_TRUE(gripper.devices[0].part.primitives[0].limits.empty());

  const Binding movel =
      ParseDevices(DeviceEntry("arm", "demo",
                               "[[profile.test.device.primitive]]\nname = \"arm.movel\"\n"
                               "version = \"1.0.0\"\n"
                               "description = \"default speed only\"\n"
                               "references = [\"relative\"]"),
                   "default-speed.toml");
  EXPECT_TRUE(movel.devices[0].part.primitives[0].limits.empty());
}

TEST(BindingTest, Ros2EndpointsValidate) {
  const Binding binding = ParseDevices(
      DeviceEntry("arm", "ros2",
                  std::string(kRos2Endpoints) + "\n\n" + Primitive("arm.movej") + "\n\n" +
                      Primitive("arm.movel") + "\n\n" + Primitive("arm.pick")),
      "ros2.toml");
  const auto& ros2 = binding.devices[0].part.ros2;
  ASSERT_TRUE(ros2.has_value());
  EXPECT_EQ(ros2->joint_states.message, "sensor_msgs/JointState");
  EXPECT_EQ(ros2->movel.name, "/goal");
  EXPECT_EQ(ros2->pick.message, "std_msgs/String");

  EXPECT_NO_THROW(
      ParseDevices(DeviceEntry("arm", "demo", Primitive("arm.pick")), "demo-pick.toml"));

  // A vendor message type is rejected: standard interfaces only.
  const std::string vendor = ParseErrorOf(DeviceEntry("arm", "ros2",
                                                      R"([profile.test.device.backend.ros2]
domain_id = 0
namespace = "/eos"

[profile.test.device.backend.ros2.joint_states]
kind = "topic"
name = "/j"
message = "vendor_msgs/JointState")"),
                                          "vendor.toml");
  EXPECT_NE(vendor.find("standard common interfaces only"), std::string::npos);

  // A service endpoint kind parses the keys but is not admitted yet.
  const std::string svc = ParseErrorOf(DeviceEntry("arm", "ros2",
                                                   R"([profile.test.device.backend.ros2]
domain_id = 0
namespace = "/eos"

[profile.test.device.backend.ros2.movel]
kind = "service"
name = "/move"
message = "std_msgs/Float64MultiArray")"),
                                       "svc.toml");
  EXPECT_NE(svc.find("not implemented yet"), std::string::npos);

  // Declaring arm.movel without a movel group fails the launch.
  const std::string cut =
      std::string(kRos2Endpoints)
          .substr(0, std::string(kRos2Endpoints).find("[profile.test.device.backend.ros2.movel]")) +
      "\n" + Primitive("arm.movel");
  const std::string missing = ParseErrorOf(DeviceEntry("arm", "ros2", cut), "missing.toml");
  EXPECT_NE(missing.find("endpoint 'movel' is required"), std::string::npos);

  // The always-required groups are checked even without commands.
  const std::string bare = ParseErrorOf(
      DeviceEntry("arm", "ros2",
                  "[profile.test.device.backend.ros2]\ndomain_id = 0\nnamespace = \"/eos\"\n"),
      "bare.toml");
  EXPECT_NE(bare.find("endpoint 'joint_states' is required"), std::string::npos);
}

TEST(BindingTest, Ros2GripperAndCameraEndpointsValidate) {
  const Binding gripper = ParseDevices(
      DeviceEntry("gripper", "ros2",
                  std::string(kRos2GripperEndpoints) + "\n\n" + Primitive("gripper.motion")),
      "gripper.toml");
  ASSERT_TRUE(gripper.devices[0].part.ros2.has_value());
  EXPECT_EQ(gripper.devices[0].part.ros2->gripper_state.name, "/eos/gripper/state");

  const Binding camera =
      ParseDevices(DeviceEntry("cam", "ros2", kRos2CameraEndpoints), "camera.toml");
  ASSERT_TRUE(camera.devices[0].part.ros2.has_value());
  EXPECT_EQ(camera.devices[0].part.ros2->image.message, "sensor_msgs/CompressedImage");
  EXPECT_EQ(camera.devices[0].part.ros2->depth_image.message, "sensor_msgs/Image");
  EXPECT_DOUBLE_EQ(camera.devices[0].part.ros2->depth_scale, 0.001);
  EXPECT_EQ(camera.devices[0].part.ros2->max_image_bytes, 4194304u);

  const std::string missing =
      ParseErrorOf(DeviceEntry("gripper", "ros2",
                               "[profile.test.device.backend.ros2]\ndomain_id = 0\n"
                               "namespace = \"/eos\"\n"),
                   "missing-gripper.toml");
  EXPECT_NE(missing.find("endpoint 'gripper_state' is required"), std::string::npos);
}

TEST(BindingTest, AplaySpeakerConfigurationAndPrimitiveValidate) {
  const Binding speaker = ParseDevices(
      DeviceEntry("speaker", "aplay",
                  std::string(kAplaySpeakerConfig) + "\n\n" + Primitive("speaker.play")),
      "speaker.toml");
  ASSERT_TRUE(speaker.devices[0].part.aplay.has_value());
  EXPECT_EQ(speaker.devices[0].part.aplay->alsa_device, "test-device");
  EXPECT_EQ(speaker.devices[0].part.aplay->max_stream_bytes, 1048576u);

  const Binding default_device =
      ParseDevices(DeviceEntry("speaker", "aplay", "[profile.test.device.backend.aplay]"),
                   "default-device.toml");
  ASSERT_TRUE(default_device.devices[0].part.aplay.has_value());
  EXPECT_EQ(default_device.devices[0].part.aplay->alsa_device, "default");
  EXPECT_EQ(default_device.devices[0].part.aplay->max_stream_bytes, 16777216u);

  const std::string missing_table =
      ParseErrorOf(DeviceEntry("speaker", "aplay"), "missing-aplay.toml");
  EXPECT_NE(missing_table.find("requires a [device.backend.aplay] table"), std::string::npos);

  const std::string empty_device = ParseErrorOf(DeviceEntry("speaker", "aplay",
                                                            "[profile.test.device.backend.aplay]\n"
                                                            "alsa_device = \"\""),
                                                "empty-device.toml");
  EXPECT_NE(empty_device.find("alsa_device' must not be empty"), std::string::npos);

  const std::string unknown_key = ParseErrorOf(
      DeviceEntry("speaker", "aplay", "[profile.test.device.backend.aplay]\nextra = true"),
      "unknown-aplay.toml");
  EXPECT_NE(unknown_key.find("unknown key 'extra'"), std::string::npos);

  const std::string zero_limit = ParseErrorOf(
      DeviceEntry("speaker", "aplay", "[profile.test.device.backend.aplay]\nmax_stream_bytes = 0"),
      "zero-stream-limit.toml");
  EXPECT_NE(zero_limit.find("max_stream_bytes' must be positive"), std::string::npos);

  const std::string ros_speaker = ParseErrorOf(
      DeviceEntry("speaker", "ros2",
                  "[profile.test.device.backend.ros2]\ndomain_id = 0\nnamespace = \"/eos\""),
      "ros-speaker.toml");
  EXPECT_NE(ros_speaker.find("only implemented for class 'arm', 'gimbal', 'gripper', or 'cam'"),
            std::string::npos);
}

TEST(BindingTest, AlsaMicConfigurationStaysCaptureOnly) {
  const Binding mic = ParseDevices(DeviceEntry("mic", "alsa", kAlsaMicConfig), "alsa-mic.toml");
  ASSERT_TRUE(mic.devices[0].part.alsa.has_value());
  EXPECT_EQ(mic.devices[0].part.alsa->alsa_device, "test-device");
  EXPECT_EQ(mic.devices[0].part.alsa->sample_rate_hz, 16000u);
  EXPECT_EQ(mic.devices[0].part.alsa->channels, 1u);
  EXPECT_EQ(mic.devices[0].part.alsa->window_frames, 640u);
  EXPECT_EQ(mic.devices[0].part.alsa->read_timeout_ms, 2000u);

  // Every key is optional: an empty table takes the audio contracts' defaults.
  const Binding defaults = ParseDevices(
      DeviceEntry("mic", "alsa", "[profile.test.device.backend.alsa]"), "alsa-defaults.toml");
  ASSERT_TRUE(defaults.devices[0].part.alsa.has_value());
  EXPECT_EQ(defaults.devices[0].part.alsa->alsa_device, "default");
  EXPECT_EQ(defaults.devices[0].part.alsa->frame_id, "mic");
  EXPECT_EQ(defaults.devices[0].part.alsa->window_frames, 320u);

  EXPECT_NE(ParseErrorOf(DeviceEntry("mic", "alsa"), "missing-alsa.toml")
                .find("requires a [device.backend.alsa] table"),
            std::string::npos);

  // Capture-only: no other class may select the backend.
  EXPECT_NE(ParseErrorOf(DeviceEntry("speaker", "alsa", "[profile.test.device.backend.alsa]"),
                         "alsa-speaker.toml")
                .find("only implemented for class 'mic'"),
            std::string::npos);

  // The backend declares no primitive of its own, and capture-only is enforced
  // by the primitive catalog rather than by this backend: no catalog primitive
  // targets class 'mic', so a capture device cannot declare one at all. That
  // keeps a future `mic.*` primitive possible without the backend having to
  // pre-declare what it cannot do.
  EXPECT_TRUE(mic.devices[0].part.primitives.empty());
  EXPECT_NE(ParseErrorOf(DeviceEntry("mic", "alsa",
                                     std::string("[profile.test.device.backend.alsa]\n") +
                                         Primitive("speaker.play")),
                         "alsa-play.toml")
                .find("belongs to class 'speaker', device class is 'mic'"),
            std::string::npos);

  // The table itself is bounded, and it may not appear under another type.
  EXPECT_NE(ParseErrorOf(
                DeviceEntry("mic", "alsa", "[profile.test.device.backend.alsa]\nwindow_frames = 0"),
                "alsa-zero.toml")
                .find("window_frames' must be within 1..48000"),
            std::string::npos);
  EXPECT_NE(
      ParseErrorOf(DeviceEntry("mic", "alsa", "[profile.test.device.backend.alsa]\nchannels = 9"),
                   "alsa-channels.toml")
          .find("channels' must be within 1..8"),
      std::string::npos);
  EXPECT_NE(
      ParseErrorOf(DeviceEntry("mic", "alsa", "[profile.test.device.backend.alsa]\nextra = true"),
                   "alsa-unknown.toml")
          .find("unknown key 'extra'"),
      std::string::npos);
  EXPECT_NE(ParseErrorOf(
                DeviceEntry("mic", "alsa", "[profile.test.device.backend.alsa]\nframe_id = \"\""),
                "alsa-frame.toml")
                .find("frame_id' must not be empty"),
            std::string::npos);
  EXPECT_NE(ParseErrorOf(DeviceEntry("mic", "demo", "[profile.test.device.backend.alsa]"),
                         "alsa-wrong-type.toml")
                .find("only valid for backend type 'alsa'"),
            std::string::npos);
}

TEST(BindingTest, V4l2CamConfigurationStaysCaptureOnly) {
  const Binding cam = ParseDevices(DeviceEntry("cam", "v4l2", kV4l2CamConfig), "v4l2-cam.toml");
  ASSERT_TRUE(cam.devices[0].part.v4l2.has_value());
  EXPECT_EQ(cam.devices[0].part.v4l2->video_device, "/dev/video2");
  EXPECT_EQ(cam.devices[0].part.v4l2->width_px, 1280u);
  EXPECT_EQ(cam.devices[0].part.v4l2->height_px, 720u);
  EXPECT_EQ(cam.devices[0].part.v4l2->fps, 15u);
  EXPECT_EQ(cam.devices[0].part.v4l2->read_timeout_ms, 3000u);

  // Every key is optional: an empty table takes the capture defaults.
  const Binding defaults = ParseDevices(
      DeviceEntry("cam", "v4l2", "[profile.test.device.backend.v4l2]"), "v4l2-defaults.toml");
  ASSERT_TRUE(defaults.devices[0].part.v4l2.has_value());
  EXPECT_EQ(defaults.devices[0].part.v4l2->video_device, "/dev/video0");
  EXPECT_EQ(defaults.devices[0].part.v4l2->frame_id, "camera");
  EXPECT_EQ(defaults.devices[0].part.v4l2->width_px, 640u);
  EXPECT_EQ(defaults.devices[0].part.v4l2->height_px, 480u);
  EXPECT_EQ(defaults.devices[0].part.v4l2->fps, 30u);
  EXPECT_EQ(defaults.devices[0].part.v4l2->max_frame_bytes, 4u * 1024u * 1024u);

  EXPECT_NE(ParseErrorOf(DeviceEntry("cam", "v4l2"), "missing-v4l2.toml")
                .find("requires a [device.backend.v4l2] table"),
            std::string::npos);

  // Capture-only: no other class may select the backend.
  EXPECT_NE(ParseErrorOf(DeviceEntry("mic", "v4l2", "[profile.test.device.backend.v4l2]"),
                         "v4l2-mic.toml")
                .find("only implemented for class 'cam'"),
            std::string::npos);

  // As with the microphone, capture-only is enforced by the primitive catalog
  // rather than by this backend: no catalog primitive targets class 'cam', so a
  // camera cannot declare one.
  EXPECT_TRUE(cam.devices[0].part.primitives.empty());
  EXPECT_NE(ParseErrorOf(DeviceEntry("cam", "v4l2",
                                     std::string("[profile.test.device.backend.v4l2]\n") +
                                         Primitive("speaker.play")),
                         "v4l2-play.toml")
                .find("belongs to class 'speaker', device class is 'cam'"),
            std::string::npos);

  // The table itself is bounded, and it may not appear under another type.
  EXPECT_NE(
      ParseErrorOf(DeviceEntry("cam", "v4l2", "[profile.test.device.backend.v4l2]\nwidth_px = 0"),
                   "v4l2-zero.toml")
          .find("width_px' must be within 1..16384"),
      std::string::npos);
  EXPECT_NE(
      ParseErrorOf(DeviceEntry("cam", "v4l2", "[profile.test.device.backend.v4l2]\nfps = 241"),
                   "v4l2-fps.toml")
          .find("fps' must be within 1..240"),
      std::string::npos);
  EXPECT_NE(
      ParseErrorOf(DeviceEntry("cam", "v4l2", "[profile.test.device.backend.v4l2]\nextra = true"),
                   "v4l2-unknown.toml")
          .find("unknown key 'extra'"),
      std::string::npos);
  EXPECT_NE(ParseErrorOf(DeviceEntry("cam", "demo", "[profile.test.device.backend.v4l2]"),
                         "v4l2-wrong-type.toml")
                .find("only valid for backend type 'v4l2'"),
            std::string::npos);
}

TEST(BindingTest, ClassAndBackendTypeSplit) {
  // The conflated selector is gone: `backend` is no longer a device key.
  const std::string conflated = ParseErrorOf(
      "[[profile.test.device]]\nclass = \"arm\"\nbackend = \"ros2-arm\"\n", "conflated.toml");
  EXPECT_NE(conflated.find("key 'backend' must be a table"), std::string::npos);
  // Removed transports are rejected rather than silently mapped to demo.
  EXPECT_NE(ParseErrorOf(DeviceEntry("arm", "sim"), "sim.toml").find("unknown backend type 'sim'"),
            std::string::npos);
  const std::string removed_vendor = ParseErrorOf(DeviceEntry("base", "vendor"), "bad.toml");
  EXPECT_NE(removed_vendor.find("unknown backend type 'vendor'"), std::string::npos);
  const std::string wrong_aplay_class =
      ParseErrorOf(DeviceEntry("arm", "aplay", kAplaySpeakerConfig), "aplay-arm.toml");
  EXPECT_NE(wrong_aplay_class.find("only implemented for class 'speaker'"), std::string::npos);
}

TEST(BindingTest, CommittedDemoRobotParses) {
  const Binding binding = LoadBinding(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo");
  ASSERT_EQ(binding.devices.size(), 7u);
  const char* const classes[] = {"base", "gimbal", "arm", "cam", "mic", "gripper", "speaker"};
  for (std::size_t i = 0; i < binding.devices.size(); ++i) {
    EXPECT_EQ(binding.devices[i].part.kind, classes[i]);
    EXPECT_EQ(binding.devices[i].backend_type, "demo");
  }
  EXPECT_EQ(binding.devices[1].part.part_id, "default");
  ASSERT_TRUE(binding.robot.model.has_value());
  EXPECT_EQ(*binding.robot.model, "eos-demo");
  EXPECT_EQ(binding.robot.id, "demo-0");
  EXPECT_EQ(binding.robot.name, "demo-0");
  ASSERT_TRUE(binding.devices[0].part.base.has_value());
  EXPECT_DOUBLE_EQ(binding.devices[0].part.base->footprint_length_m, 0.5);
  EXPECT_DOUBLE_EQ(binding.devices[0].part.base->footprint_width_m, 0.3);
  ASSERT_EQ(binding.devices[0].part.primitives[1].references.size(), 2u);
  EXPECT_EQ(binding.devices[0].part.primitives[1].references[0], "relative");
  EXPECT_EQ(binding.devices[0].part.primitives[1].references[1], "absolute");
  EXPECT_EQ(binding.devices[0].part.primitives[1].absolute_frame, "map");

  const auto& gimbal = binding.devices[1];

  const auto& arm = binding.devices[2];
  ASSERT_EQ(arm.part.primitives.size(), 4u);
  EXPECT_EQ(arm.part.primitives[0].name, "arm.init");
  EXPECT_EQ(arm.part.primitives[1].name, "arm.movej");
  EXPECT_EQ(arm.part.primitives[2].name, "arm.movel");
  EXPECT_EQ(arm.part.primitives[3].name, "arm.pick");
  ASSERT_TRUE(arm.part.arm.has_value());
  EXPECT_DOUBLE_EQ(*arm.part.arm->link_lengths_m[0], 0.7);
  ASSERT_EQ(arm.part.arm->initial_joint_positions_rad.size(), 6u);
  EXPECT_DOUBLE_EQ(arm.part.arm->initial_joint_positions_rad[0], 0.0);

  // The gripper and camera declare their respective primitives.
  EXPECT_EQ(binding.devices[5].part.primitives.size(), 1u);
  EXPECT_EQ(binding.devices[5].part.primitives[0].name, "gripper.motion");
  ASSERT_EQ(binding.devices[3].part.primitives.size(), 1u);
  EXPECT_EQ(binding.devices[3].part.primitives[0].name, "face.register");
  EXPECT_EQ(binding.devices[3].part.primitives[0].model_id, "arcface-register");
  ASSERT_EQ(binding.devices[6].part.primitives.size(), 2u);
  EXPECT_EQ(binding.devices[6].part.primitives[1].model_id, "audio8");
  EXPECT_FALSE(binding.devices[4].part.alsa.has_value());
  EXPECT_FALSE(binding.devices[6].part.aplay.has_value());
}

TEST(BindingTest, HostProfileSwapsOnlyTheCamera) {
  const Binding demo = LoadBinding(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo");
  const Binding host =
      LoadBinding(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo-host");

  // demo-host is a copy of demo with its camera capture replaced; both use
  // ALSA for the local microphone and system-default speaker. This test keeps
  // the copy honest: a device, primitive, or feed rate changed in one file and
  // not the other fails here rather than drifting silently.
  ASSERT_EQ(host.devices.size(), demo.devices.size());
  EXPECT_EQ(host.robot.model, demo.robot.model);
  for (std::size_t index = 0; index < demo.devices.size(); ++index) {
    const auto& original = demo.devices[index];
    const auto& variant = host.devices[index];
    EXPECT_EQ(variant.part.kind, original.part.kind) << index;
    EXPECT_EQ(variant.part.part_id, original.part.part_id) << index;
    EXPECT_EQ(variant.part.primitives.size(), original.part.primitives.size()) << index;
    EXPECT_DOUBLE_EQ(variant.envelope.geometric.max_rate_hz,
                     original.envelope.geometric.max_rate_hz)
        << index;
    EXPECT_DOUBLE_EQ(variant.envelope.proprioception.max_rate_hz,
                     original.envelope.proprioception.max_rate_hz)
        << index;
    EXPECT_DOUBLE_EQ(variant.envelope.sensor.max_rate_hz, original.envelope.sensor.max_rate_hz)
        << index;
    EXPECT_DOUBLE_EQ(variant.envelope.health.max_rate_hz, original.envelope.health.max_rate_hz)
        << index;
    const bool camera = original.part.kind == "cam";
    const bool microphone = original.part.kind == "mic";
    const bool speaker = original.part.kind == "speaker";
    EXPECT_EQ(variant.backend_type,
              camera ? "v4l2" : (microphone ? "alsa" : (speaker ? "aplay" : "demo")))
        << index;
    EXPECT_EQ(variant.part.alsa.has_value(), microphone) << index;
    EXPECT_EQ(variant.part.aplay.has_value(), speaker) << index;
    EXPECT_EQ(variant.part.v4l2.has_value(), camera) << index;
  }

  // Host camera contract defaults; its feed rates match the demo camera.
  const auto& cam = host.devices[3];
  ASSERT_EQ(cam.part.kind, "cam");
  ASSERT_TRUE(cam.part.v4l2.has_value());
  EXPECT_EQ(cam.part.v4l2->video_device, "/dev/video0");
  EXPECT_EQ(cam.part.v4l2->width_px, 640u);
  EXPECT_EQ(cam.part.v4l2->height_px, 480u);
  EXPECT_EQ(cam.part.v4l2->fps, 30u);
  EXPECT_DOUBLE_EQ(cam.envelope.sensor.max_rate_hz, 30.0);

  const auto& mic = host.devices[4];
  ASSERT_EQ(mic.part.kind, "mic");
  ASSERT_TRUE(mic.part.alsa.has_value());
  EXPECT_EQ(mic.part.alsa->alsa_device, "default");
  EXPECT_EQ(mic.part.alsa->sample_rate_hz, 16000u);
  EXPECT_EQ(mic.part.alsa->channels, 1u);
  EXPECT_EQ(mic.part.alsa->window_frames, 320u);
  EXPECT_DOUBLE_EQ(mic.envelope.sensor.max_rate_hz, 250.0);

  const auto& speaker = host.devices[6];
  ASSERT_EQ(speaker.part.kind, "speaker");
  ASSERT_TRUE(speaker.part.aplay.has_value());
  EXPECT_EQ(speaker.part.aplay->alsa_device, "default");
}

TEST(BindingTest, RemovedDemoSubprofilesAreUnavailable) {
  const char* const removed[] = {"demo_arm", "demo_base",    "demo_gimbal", "demo_cam",
                                 "demo_mic", "demo_gripper", "demo_speaker"};
  for (const char* profile : removed) {
    std::string error;
    try {
      LoadBinding(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", profile);
    } catch (const std::runtime_error& exception) {
      error = exception.what();
    }
    EXPECT_NE(error.find("profile '" + std::string(profile) + "' not found"), std::string::npos);
  }
}

TEST(BindingTest, MissingFeedClassFails) {
  const std::string full = DeviceEntry("base", "demo");
  const std::string error =
      ParseErrorOf(full.substr(0, full.find("[profile.test.device.feeds.health]")), "short.toml");
  EXPECT_NE(error.find("short.toml"), std::string::npos);
  EXPECT_NE(error.find("missing feed class 'health'"), std::string::npos);
}

TEST(BindingTest, UnknownFeedClassFails) {
  const std::string error = ParseErrorOf(
      DeviceEntry("base", "demo") + "\n[profile.test.device.feeds.telemetry]\nmax_rate_hz = 5.0\n",
      "typo.toml");
  EXPECT_NE(error.find("typo.toml"), std::string::npos);
  EXPECT_NE(error.find("unknown feed class 'telemetry'"), std::string::npos);
}

TEST(BindingTest, LegacyModelFeedClassIsRejected) {
  const std::string error = ParseErrorOf(
      DeviceEntry("base", "demo") + "\n[profile.test.device.feeds.model]\nmax_rate_hz = 5.0\n",
      "legacy-model-feed.toml");
  EXPECT_NE(error.find("unknown feed class 'model'"), std::string::npos);
}

TEST(BindingTest, LegacyMaxBytesIsRejected) {
  std::string config = DeviceEntry("base", "demo");
  config += "max_bytes = 2\n";
  const std::string error = ParseErrorOf(config, "legacy-bytes.toml");
  EXPECT_NE(error.find("legacy-bytes.toml"), std::string::npos);
  EXPECT_NE(error.find("unknown key 'max_bytes'"), std::string::npos);
}

TEST(BindingTest, EmptyProfileFails) {
  const std::string error = ParseErrorOf("# nothing here\n", "empty.toml");
  EXPECT_NE(error.find("empty.toml"), std::string::npos);
  EXPECT_NE(error.find("no '[profile.<name>]' tables"), std::string::npos);
}

TEST(BindingTest, UnknownDeviceKeyFails) {
  const std::string error =
      ParseErrorOf(DeviceEntry("base", "demo", "speed_limit = 2.0"), "key.toml");
  EXPECT_NE(error.find("key.toml"), std::string::npos);
  EXPECT_NE(error.find("unknown key 'speed_limit'"), std::string::npos);
}

TEST(BindingTest, JointNamesMustBeNonEmptyAndDistinct) {
  const std::string duplicate =
      ParseErrorOf(DeviceEntry("arm", "demo", "joint_names = [\"joint1\", \"joint1\"]"),
                   "duplicate-joints.toml");
  EXPECT_NE(duplicate.find("distinct joint names"), std::string::npos);

  const std::string empty = ParseErrorOf(
      DeviceEntry("arm", "demo", "joint_names = [\"joint1\", \"\"]"), "empty-joint.toml");
  EXPECT_NE(empty.find("empty joint names"), std::string::npos);
}

TEST(BindingTest, UnknownPartKeyFails) {
  const std::string error = ParseErrorOf(
      DeviceEntry("base", "demo", "[profile.test.device.part]\nspeed_limit = 2.0"), "part.toml");
  EXPECT_NE(error.find("part.toml"), std::string::npos);
  EXPECT_NE(error.find("unknown key 'speed_limit'"), std::string::npos);
}

TEST(BindingTest, LegacySceneObjectsAreRejected) {
  const std::string error = ParseErrorOf(DeviceEntry("gimbal", "demo",
                                                     R"([[profile.test.device.scene.objects]]
id = "object-1"
label = "crate"
pickable = true
x_m = 1.0
y_m = 0.0)"),
                                         "scene.toml");
  EXPECT_NE(error.find("scene.toml"), std::string::npos);
  EXPECT_NE(error.find("unknown key 'scene'"), std::string::npos);
}

TEST(BindingTest, PartConfigDefaultsApplyWithoutPartTable) {
  const Binding binding = ParseDevices(DeviceEntry("base", "demo") + DeviceEntry("gimbal", "demo") +
                                           DeviceEntry("arm", "demo") + DeviceEntry("cam", "demo"),
                                       "defaults.toml");
  ASSERT_EQ(binding.devices.size(), 4u);

  // Each class's member is populated with its documented defaults even when
  // the binding omits [device.part]; other classes' members stay absent.
  const auto& base = binding.devices[0].part;
  ASSERT_TRUE(base.base.has_value());
  EXPECT_DOUBLE_EQ(base.base->footprint_length_m, 0.5);
  EXPECT_FALSE(base.gimbal.has_value());

  const auto& gimbal = binding.devices[1].part;
  ASSERT_TRUE(gimbal.gimbal.has_value());
  EXPECT_DOUBLE_EQ(gimbal.gimbal->fov_rad, 1.4);
  EXPECT_DOUBLE_EQ(gimbal.gimbal->range_m, 5.0);

  const auto& arm = binding.devices[2].part;
  ASSERT_TRUE(arm.arm.has_value());
  const double expected[6] = {0.18, 0.16, 0.12, -1.0, -1.0, -1.0};  // -1 = nullopt
  for (std::size_t i = 0; i < 6; ++i) {
    ASSERT_TRUE(arm.arm->link_lengths_m[i].has_value() == (expected[i] > 0.0)) << i;
    if (expected[i] > 0.0) {
      EXPECT_DOUBLE_EQ(*arm.arm->link_lengths_m[i], expected[i]);
    }
  }

  const auto& cam = binding.devices[3].part;
  ASSERT_TRUE(cam.camera.has_value());
  ASSERT_TRUE(cam.camera->fov_rad.has_value());
  EXPECT_DOUBLE_EQ(*cam.camera->fov_rad, 1.4);
}

TEST(BindingTest, PartConfigParsesTypedKeys) {
  const Binding binding = ParseDevices(
      DeviceEntry("base", "demo",
                  "[profile.test.device.part]\nfootprint_length_m = 0.8\nfootprint_width_m = 0.6") +
          DeviceEntry("gimbal", "demo",
                      "[profile.test.device.part]\nfov_rad = 1.0\nrange_m = 8.0") +
          DeviceEntry(
              "arm", "demo",
              "[profile.test.device.part]\nlink_lengths_m = [0.2, 0.15]\nmount_x_m = -0.1") +
          DeviceEntry("cam", "demo", "[profile.test.device.part]\nfov_rad = 2.1"),
      "parts.toml");
  ASSERT_EQ(binding.devices.size(), 4u);

  ASSERT_TRUE(binding.devices[2].part.arm.has_value());
  const auto& links = binding.devices[2].part.arm->link_lengths_m;
  ASSERT_TRUE(links[0].has_value() && links[1].has_value());
  EXPECT_DOUBLE_EQ(*links[1], 0.15);
  for (std::size_t i = 2; i < links.size(); ++i) {
    EXPECT_FALSE(links[i].has_value()) << i;
  }
  EXPECT_DOUBLE_EQ(binding.devices[2].part.arm->mount_x_m, -0.1);

  // Classes without [device.part] keys keep failing loudly.
  const std::string mic_typo = ParseErrorOf(
      DeviceEntry("mic", "demo", "[profile.test.device.part]\nfov_rad = 1.0"), "mic.toml");
  EXPECT_NE(mic_typo.find("unknown key 'fov_rad'"), std::string::npos);
}

}  // namespace
