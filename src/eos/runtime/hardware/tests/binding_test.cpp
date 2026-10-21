/// Binding parser tests for shared profile syntax, geometry, feeds, and
/// primitive admission. Adapter configuration is tested with its backend.
#include "binding.h"
#include "test_backend.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using eos::runtime::hardware_runtime::Binding;
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
  return ParseBinding(devices, path, "test", eos::runtime::hardware_runtime::test::Catalog());
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
/// synthetic feed rates, plus the given extra `body`. `body` lines must use
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

TEST(BindingTest, CatalogRejectsDuplicateRegistration) {
  auto catalog = eos::runtime::hardware_runtime::test::Catalog();
  eos::runtime::hardware_runtime::BackendDescriptor duplicate;
  duplicate.type = "test";
  EXPECT_THROW(catalog.Register(std::move(duplicate)), std::invalid_argument);
}

TEST(BindingTest, CatalogRejectsUnknownBackendAndUnsupportedClass) {
  EXPECT_NE(ParseErrorOf(DeviceEntry("base", "missing"), "unknown.toml")
                .find("unknown backend type 'missing'"), std::string::npos);
  const std::string error = [&] {
    try {
      ParseBinding(DeviceEntry("arm", "test"), "class.toml", "test",
                   eos::runtime::hardware_runtime::test::Catalog(true, true));
    } catch (const std::runtime_error& exception) {
      return std::string(exception.what());
    }
    return std::string{};
  }();
  EXPECT_NE(error.find("class"), std::string::npos);
  EXPECT_NE(error.find("arm"), std::string::npos);
}

TEST(BindingTest, DescriptorRejectionKeepsBindingContext) {
  const std::string error = ParseErrorOf(
      DeviceEntry("base", "test", "[profile.test.device.backend.test]\nextra = true"),
      "settings.toml");
  EXPECT_NE(error.find("settings.toml"), std::string::npos);
  EXPECT_NE(error.find("test backend rejected settings"), std::string::npos);
}

TEST(BindingTest, SingleDeviceProfileParses) {
  const std::string identity =
      "part_id = \"default\"\ndescription = \"left drive\"\ntags = [\"locomotion\"]";
  const Binding binding = ParseDevices(DeviceEntry("base", "test",
                                                   identity + "\n\n" + Primitive("base.velocity") +
                                                       "\n\n" + Primitive("base.posture")),
                                       "inline.toml");
  ASSERT_EQ(binding.devices.size(), 1u);
  const auto& device = binding.devices[0];
  EXPECT_EQ(device.backend_type, "test");
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
      ParseErrorOf(DeviceEntry("arm", "test", Primitive("arm.init")), "arm-init-missing.toml");
  EXPECT_NE(missing.find("initial_joint_positions_rad"), std::string::npos);

  const std::string mismatch =
      ParseErrorOf(DeviceEntry("arm", "test",
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
    ParseBinding(DeviceEntry("base", "test"), "inline.toml", "", eos::runtime::hardware_runtime::test::Catalog());
  } catch (const std::runtime_error& e) {
    error = e.what();
  }
  EXPECT_NE(error.find("a profile is required"), std::string::npos);

  // The legacy single-device shape no longer parses: it is not a profiled
  // file at all.
  const std::string legacy =
      ParseErrorOf("backend = \"test-base\"\ndevice_id = \"x\"\n", "legacy.toml");
  EXPECT_NE(legacy.find("no '[profile.<name>]' tables"), std::string::npos);
}

TEST(BindingTest, UnknownProfileFails) {
  std::string error;
  try {
    ParseBinding(DeviceEntry("base", "test"), "inline.toml", "ghost", eos::runtime::hardware_runtime::test::Catalog());
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
          DeviceEntry("base", "test"),
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
      "[profile.test.robot]\nid = \"robot-a\"\n" + DeviceEntry("base", "test"), "robot-id.toml");
  EXPECT_NE(robot.find("rename it to 'robot_id'"), std::string::npos);
}

TEST(BindingTest, JointNamesAreChannelOrderOnly) {
  // The binding carries joint NAMES (channel order for the graph), never
  // ranges/types/limits — that is URDF data.
  const Binding binding = ParseDevices(
      DeviceEntry("arm", "test", "joint_names = [\"j_a\", \"j_b\"]\n\n" +
                                     Primitive("arm.movej")),
      "names.toml");
  const auto& part = binding.devices[0].part;
  ASSERT_EQ(part.joint_names.size(), 2u);
  EXPECT_EQ(part.joint_names[0], "j_a");

  // A key list is for jointed classes only, and 1..6 entries.
  EXPECT_NE(ParseErrorOf(DeviceEntry("base", "test", "joint_names = [\"j\"]"), "cross.toml")
                .find("only valid for class 'arm' or 'gimbal'"),
            std::string::npos);
  EXPECT_NE(ParseErrorOf(DeviceEntry("arm", "test", "joint_names = []"), "empty.toml")
                .find("1..6 joint names"),
            std::string::npos);
}

TEST(BindingTest, RejectsDuplicatePartIdsWithinOneDeviceClass) {
  const std::string error = ParseErrorOf(DeviceEntry("cam", "test") + DeviceEntry("cam", "test"),
                                         "duplicate-camera-part.toml");
  EXPECT_NE(error.find("duplicate cam part_id 'default'"), std::string::npos) << error;
}

TEST(BindingTest, RemovedWorkspaceDefaultsAreRejected) {
  EXPECT_NE(
      ParseErrorOf(DeviceEntry("arm", "test", "[profile.test.device.workspace]\nplane_z_m = 0.3\n"),
                   "workspace.toml")
          .find("unknown key 'workspace'"),
      std::string::npos);
  // Nor do 3-D mounts.
  EXPECT_NE(
      ParseErrorOf(DeviceEntry("arm", "test", "[profile.test.device.mount]\non = \"robot\"\n"),
                   "mount.toml")
          .find("unknown key 'mount'"),
      std::string::npos);

  EXPECT_NE(
      ParseErrorOf(DeviceEntry("arm", "test", "[profile.test.device.motion]\nspeed_scale = 10\n"),
                   "motion.toml")
          .find("unknown key 'motion'"),
      std::string::npos);
}

TEST(BindingTest, PrimitiveAdmissionChecks) {
  const std::string missing_description = ParseErrorOf(
      DeviceEntry(
          "base", "test",
          "[[profile.test.device.primitive]]\nname = \"base.velocity\"\nversion = \"1.0.0\""),
      "description.toml");
  EXPECT_NE(missing_description.find("missing required key 'description'"), std::string::npos);
  const Binding empty_description = ParseDevices(
      DeviceEntry(
          "base", "test",
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
          "base", "test",
          "[[profile.test.device.primitive]]\nname = \"base.velocity\"\ndescription = \"\""),
      "version.toml");
  EXPECT_NE(missing_version.find("missing required key 'version'"), std::string::npos);
  const std::string wrong_version = ParseErrorOf(
      DeviceEntry(
          "base", "test",
          "[[profile.test.device.primitive]]\nname = \"base.velocity\"\nversion = \"2.0.0\"\n"
          "description = \"\""),
      "wrong-version.toml");
  EXPECT_NE(wrong_version.find("unknown primitive 'base.velocity' version '2.0.0'"),
            std::string::npos);

  const std::string unknown_primitive =
      ParseErrorOf(DeviceEntry("arm", "test", Primitive("arm.wave")), "unknown.toml");
  EXPECT_NE(unknown_primitive.find("unknown primitive 'arm.wave'"), std::string::npos);
  const std::string privileged =
      ParseErrorOf(DeviceEntry("arm", "test", Primitive("safety.estop")), "priv.toml");
  EXPECT_NE(privileged.find("privileged primitive 'safety.estop' is implicit"), std::string::npos);
  const std::string cross_class =
      ParseErrorOf(DeviceEntry("base", "test", Primitive("arm.movej")), "cross.toml");
  EXPECT_NE(cross_class.find("belongs to class 'arm'"), std::string::npos);
  // A removed transport fails before the command is admitted.
  const std::string wrong_type =
      ParseErrorOf(DeviceEntry("arm", "sim", Primitive("arm.movej")), "types.toml");
  EXPECT_NE(wrong_type.find("unknown backend type 'sim'"), std::string::npos);
  const std::string wrong_tier = ParseErrorOf(
      DeviceEntry("arm", "test", Primitive("arm.movej") + "\ntier = \"standard\""), "tier.toml");
  EXPECT_NE(wrong_tier.find("catalog fixes the other tier"), std::string::npos);
}

TEST(BindingTest, AdvancedPrimitiveRequiresExactModelBinding) {
  const std::string missing = ParseErrorOf(
      DeviceEntry("speaker", "test", Primitive("speaker.speak")), "advanced-missing-model.toml");
  EXPECT_NE(missing.find("advanced primitive 'speaker.speak' requires a non-empty model_id"),
            std::string::npos);

  const Binding binding = ParseDevices(
      DeviceEntry("speaker", "test", Primitive("speaker.speak") + "\nmodel_id = \"tts-main\""),
      "advanced.toml");
  ASSERT_EQ(binding.devices[0].part.primitives.size(), 1u);
  EXPECT_EQ(binding.devices[0].part.primitives[0].model_id, "tts-main");

  const std::string direct = ParseErrorOf(
      DeviceEntry("speaker", "test", Primitive("speaker.play") + "\nmodel_id = \"tts-main\""),
      "direct-model.toml");
  EXPECT_NE(direct.find("model_id' is only valid for advanced primitives"), std::string::npos);
  const std::string empty_direct =
      ParseErrorOf(DeviceEntry("speaker", "test", Primitive("speaker.play") + "\nmodel_id = \"\""),
                   "direct-empty-model.toml");
  EXPECT_NE(empty_direct.find("model_id' is only valid for advanced primitives"),
            std::string::npos);
}

TEST(BindingTest, MotionReferencesAreExplicitAndValidated) {
  const Binding relative = ParseDevices(DeviceEntry("base", "test", Primitive("base.navigate")),
                                        "relative-reference.toml");
  EXPECT_EQ(relative.devices[0].part.primitives[0].references,
            std::vector<std::string>({"relative"}));
  EXPECT_FALSE(relative.devices[0].part.primitives[0].absolute_frame.has_value());

  const std::string missing =
      ParseErrorOf(DeviceEntry("base", "test",
                               "[[profile.test.device.primitive]]\nname = \"base.navigate\"\n"
                               "version = \"1.0.0\"\ndescription = \"missing references\"\n"
                               "limits = { x_m = { minimum = -1.0, maximum = 1.0 }, "
                               "y_m = { minimum = -1.0, maximum = 1.0 }, "
                               "yaw_rad = { minimum = -1.0, maximum = 1.0 } }"),
                   "missing-references.toml");
  EXPECT_NE(missing.find("requires key 'references'"), std::string::npos);

  const std::string missing_frame =
      ParseErrorOf(DeviceEntry("base", "test",
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
      DeviceEntry("arm", "test",
                  "[[profile.test.device.primitive]]\nname = \"arm.movej\"\n"
                  "version = \"1.0.0\"\ndescription = \"duplicate references\"\n"
                  "references = [\"relative\", \"relative\"]\n"
                  "limits = { joint_positions_rad = { minimum = -3.14, maximum = 3.14 } }"),
      "duplicate-reference.toml");
  EXPECT_NE(duplicate.find("duplicate value 'relative'"), std::string::npos);
}

TEST(BindingTest, PrimitiveLimitsAreResolvedAndValidated) {
  const Binding binding =
      ParseDevices(DeviceEntry("base", "test", Primitive("base.velocity")), "limits.toml");
  const auto& limits = binding.devices[0].part.primitives[0].limits;
  ASSERT_EQ(limits.size(), 4u);
  ASSERT_TRUE(limits.at("linear_x_m_s").minimum.has_value());
  ASSERT_TRUE(limits.at("linear_x_m_s").maximum.has_value());
  EXPECT_DOUBLE_EQ(*limits.at("linear_x_m_s").minimum, -2.0);
  EXPECT_DOUBLE_EQ(*limits.at("linear_x_m_s").maximum, 2.0);
  EXPECT_TRUE(limits.at("linear_x_m_s").Contains(2.0));
  EXPECT_FALSE(limits.at("linear_x_m_s").Contains(2.1));

  const std::string missing =
      ParseErrorOf(DeviceEntry("base", "test",
                               "[[profile.test.device.primitive]]\nname = \"base.velocity\"\n"
                               "version = \"1.0.0\"\n"
                               "description = \"missing one range\"\n"
                               "limits = { linear_x_m_s = { minimum = -1.0, maximum = 1.0 }, "
                               "linear_y_m_s = { minimum = -1.0, maximum = 1.0 }, "
                               "valid_for_ms = { minimum = 1.0, maximum = 1000.0 } }"),
                   "missing-limit.toml");
  EXPECT_NE(missing.find("missing required limit 'angular_z_rad_s'"), std::string::npos);

  const std::string inverted = ParseErrorOf(
      DeviceEntry("arm", "test",
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
      ParseDevices(DeviceEntry("gripper", "test",
                               "[[profile.test.device.primitive]]\nname = \"gripper.motion\"\n"
                               "version = \"1.0.0\"\n"
                               "description = \"default force only\""),
                   "default-force.toml");
  EXPECT_TRUE(gripper.devices[0].part.primitives[0].limits.empty());

  const Binding movel =
      ParseDevices(DeviceEntry("arm", "test",
                               "[[profile.test.device.primitive]]\nname = \"arm.movel\"\n"
                               "version = \"1.0.0\"\n"
                               "description = \"default speed only\"\n"
                               "references = [\"relative\"]"),
                   "default-speed.toml");
  EXPECT_TRUE(movel.devices[0].part.primitives[0].limits.empty());
}

TEST(BindingTest, MissingFeedClassFails) {
  const std::string full = DeviceEntry("base", "test");
  const std::string error =
      ParseErrorOf(full.substr(0, full.find("[profile.test.device.feeds.health]")), "short.toml");
  EXPECT_NE(error.find("short.toml"), std::string::npos);
  EXPECT_NE(error.find("missing feed class 'health'"), std::string::npos);
}

TEST(BindingTest, UnknownFeedClassFails) {
  const std::string error = ParseErrorOf(
      DeviceEntry("base", "test") + "\n[profile.test.device.feeds.telemetry]\nmax_rate_hz = 5.0\n",
      "typo.toml");
  EXPECT_NE(error.find("typo.toml"), std::string::npos);
  EXPECT_NE(error.find("unknown feed class 'telemetry'"), std::string::npos);
}

TEST(BindingTest, LegacyModelFeedClassIsRejected) {
  const std::string error = ParseErrorOf(
      DeviceEntry("base", "test") + "\n[profile.test.device.feeds.model]\nmax_rate_hz = 5.0\n",
      "legacy-model-feed.toml");
  EXPECT_NE(error.find("unknown feed class 'model'"), std::string::npos);
}

TEST(BindingTest, LegacyMaxBytesIsRejected) {
  std::string config = DeviceEntry("base", "test");
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
      ParseErrorOf(DeviceEntry("base", "test", "speed_limit = 2.0"), "key.toml");
  EXPECT_NE(error.find("key.toml"), std::string::npos);
  EXPECT_NE(error.find("unknown key 'speed_limit'"), std::string::npos);
}

TEST(BindingTest, JointNamesMustBeNonEmptyAndDistinct) {
  const std::string duplicate =
      ParseErrorOf(DeviceEntry("arm", "test", "joint_names = [\"joint1\", \"joint1\"]"),
                   "duplicate-joints.toml");
  EXPECT_NE(duplicate.find("distinct joint names"), std::string::npos);

  const std::string empty = ParseErrorOf(
      DeviceEntry("arm", "test", "joint_names = [\"joint1\", \"\"]"), "empty-joint.toml");
  EXPECT_NE(empty.find("empty joint names"), std::string::npos);
}

TEST(BindingTest, UnknownPartKeyFails) {
  const std::string error = ParseErrorOf(
      DeviceEntry("base", "test", "[profile.test.device.part]\nspeed_limit = 2.0"), "part.toml");
  EXPECT_NE(error.find("part.toml"), std::string::npos);
  EXPECT_NE(error.find("unknown key 'speed_limit'"), std::string::npos);
}

TEST(BindingTest, LegacySceneObjectsAreRejected) {
  const std::string error = ParseErrorOf(DeviceEntry("gimbal", "test",
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
  const Binding binding = ParseDevices(DeviceEntry("base", "test") + DeviceEntry("gimbal", "test") +
                                           DeviceEntry("arm", "test") + DeviceEntry("cam", "test"),
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
      DeviceEntry("base", "test",
                  "[profile.test.device.part]\nfootprint_length_m = 0.8\nfootprint_width_m = 0.6") +
          DeviceEntry("gimbal", "test",
                      "[profile.test.device.part]\nfov_rad = 1.0\nrange_m = 8.0") +
          DeviceEntry(
              "arm", "test",
              "[profile.test.device.part]\nlink_lengths_m = [0.2, 0.15]\nmount_x_m = -0.1") +
          DeviceEntry("cam", "test", "[profile.test.device.part]\nfov_rad = 2.1"),
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
      DeviceEntry("mic", "test", "[profile.test.device.part]\nfov_rad = 1.0"), "mic.toml");
  EXPECT_NE(mic_typo.find("unknown key 'fov_rad'"), std::string::npos);
}

}  // namespace
