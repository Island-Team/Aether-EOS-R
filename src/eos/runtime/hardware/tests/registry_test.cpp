/// Registry tests: factory admission of the committed bindings, startup
/// admission failures that name device/feed/values, tick integration through
/// the concrete DemoBase, the demo-scale typed accessors, and health aggregation.
#include "registry.h"

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "src/eos/ext/backends/demo/arm/demo_arm.h"
#include "src/eos/ext/backends/demo/base/demo_base.h"
#include "src/eos/ext/backends/demo/cam/demo_cam.h"
#include "src/eos/ext/backends/demo/gimbal/demo_gimbal.h"
#include "src/eos/ext/backends/demo/gripper/demo_gripper.h"
#include "src/eos/ext/backends/demo/mic/demo_mic.h"
#include "src/eos/ext/backends/demo/speaker/demo_speaker.h"

namespace {

using eos::hardware::DeviceKind;
using eos::runtime::hardware_runtime::Binding;
using eos::runtime::hardware_runtime::Device;
using eos::runtime::hardware_runtime::ParseBinding;
using eos::runtime::hardware_runtime::Registry;

static_assert(
    std::is_same_v<decltype(std::declval<const Device&>().feeds()), const eos::hardware::Backend&>);
static_assert(
    std::is_same_v<decltype(std::declval<Registry&>().Find(DeviceKind::kBase)), const Device&>);

constexpr const char* kRuntimeId = "018f22d2-82ab-7def-9234-56789abcdef0";

/// Single demo-base binding for tick and halt paths.
const char* kBaseBinding = R"toml(
[[profile.test.device]]
class = "base"

[profile.test.device.backend]
type = "demo"

[profile.test.device.feeds.geometric]
max_rate_hz = 0.0

[profile.test.device.feeds.proprioception]
max_rate_hz = 100.0

[profile.test.device.feeds.sensor]
max_rate_hz = 0.0

[profile.test.device.feeds.health]
max_rate_hz = 10.0
)toml";

const char* kAplayBinding = R"toml(
[[profile.test.device]]
class = "speaker"

[[profile.test.device.primitive]]
name = "speaker.play"
version = "1.0.0"
description = "Play synthesized speech."

[profile.test.device.backend]
type = "aplay"

[profile.test.device.backend.aplay]
alsa_device = "default"
max_stream_bytes = 1048576

[profile.test.device.feeds.proprioception]
max_rate_hz = 10.0

[profile.test.device.feeds.health]
max_rate_hz = 10.0
)toml";

/// Runs `body` and returns the error message; "" when nothing threw.
template <typename Body>
std::string ErrorOf(Body body) {
  try {
    body();
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return "";
}

TEST(RegistryTest, AdmitsCommittedRobotBinding) {
  Registry registry =
      Registry::Load(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo", kRuntimeId);
  ASSERT_EQ(registry.devices().size(), 7u);
  for (const auto& device : registry.devices()) {
    EXPECT_TRUE(device.ReadHealth().alive);
    EXPECT_TRUE(device.ReadHealth().admitted);
  }

  EXPECT_EQ(registry.robot().id, kRuntimeId);
  EXPECT_EQ(registry.Find(DeviceKind::kBase).identity().device_id, "base-0");
  EXPECT_EQ(registry.Find(DeviceKind::kGimbal).identity().device_id, "gimbal-0");
  EXPECT_EQ(registry.Find(DeviceKind::kManipulator).identity().device_id, "arm-0");
  EXPECT_EQ(registry.Find(DeviceKind::kCamera).identity().device_id, "cam-0");
  EXPECT_EQ(registry.Find(DeviceKind::kMicrophone).identity().device_id, "mic-0");
  EXPECT_EQ(registry.Find(DeviceKind::kGripper).identity().device_id, "gripper-0");
  EXPECT_EQ(registry.Find(DeviceKind::kSpeaker).identity().device_id, "speaker-0");

  // By kind plus part_id, and the error path when the part is absent.
  auto& base = registry.Find(DeviceKind::kBase, "default");
  EXPECT_EQ(base.part().part_id, "default");
  const std::string error = ErrorOf([&] { registry.Find(DeviceKind::kBase, "left"); });
  EXPECT_NE(error.find("left"), std::string::npos);
}

TEST(RegistryTest, ReusesConfiguredRobotIdAcrossRestarts) {
  const std::string path = std::string(EOS_SOURCE_ETC) + "/backends/demo.toml";
  Registry first = Registry::Load(path, "demo");
  Registry second = Registry::Load(path, "demo");

  EXPECT_EQ(first.robot().id, "demo-0");
  EXPECT_EQ(second.robot().id, first.robot().id);
  for (const auto& device : first.devices()) {
    EXPECT_EQ(device.robot_id(), first.robot().id);
  }
  for (const auto& device : second.devices()) {
    EXPECT_EQ(device.robot_id(), second.robot().id);
  }
}

TEST(RegistryTest, AdmitsAplaySpeakerWithoutModelServices) {
  const Binding binding = ParseBinding(kAplayBinding, "aplay.toml", "test");
  Registry registry = Registry::Adopt(binding, kRuntimeId);
  EXPECT_EQ(registry.Find(DeviceKind::kSpeaker).identity().name, "aplay-speaker");
}

TEST(RegistryTest, RejectsMultipleBindingsPerProcess) {
  Binding first = ParseBinding(kBaseBinding, "first.toml", "test");
  Binding second = ParseBinding(kBaseBinding, "second.toml", "test");
  const std::string error = ErrorOf([&] {
    Registry::Adopt(std::vector<Binding>{std::move(first), std::move(second)});
  });
  EXPECT_NE(error.find("exactly one hardware binding"), std::string::npos);
}

TEST(RegistryTest, TypedAccessorsReturnDemosOrNull) {
  Registry registry = Registry::Load(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo");

  // Every demo-backed kind resolves through its typed accessor.
  ASSERT_NE(registry.Base(), nullptr);
  EXPECT_EQ(registry.Base()->Identify().device_id, "base-0");
  ASSERT_NE(registry.Arm(), nullptr);
  EXPECT_EQ(registry.Arm()->Identify().device_id, "arm-0");
  ASSERT_NE(registry.Gimbal(), nullptr);
  EXPECT_EQ(registry.Gimbal()->Identify().device_id, "gimbal-0");
  ASSERT_NE(registry.Gripper(), nullptr);
  EXPECT_EQ(registry.Gripper()->Identify().device_id, "gripper-0");
  ASSERT_NE(registry.Speaker(), nullptr);
  EXPECT_EQ(registry.Speaker()->Identify().device_id, "speaker-0");
  ASSERT_NE(registry.FindDevice("speaker-0"), nullptr);
  EXPECT_EQ(registry.FindDevice("speaker-0")->identity().name, "demo-speaker");

  // A part_id that is not in the binding returns nullptr, not a guess.
  EXPECT_EQ(registry.Base("left"), nullptr);
  EXPECT_EQ(registry.Gripper("spare"), nullptr);

  // The accessors expose the working API: release through the gripper.
  eos::hardware::GripperCommand cmd;
  cmd.action = eos::hardware::GripperAction::kClose;
  cmd.force_n = 7.0;
  ASSERT_EQ(const_cast<eos::backends::demo::DemoGripper*>(registry.Gripper())->ExecuteGripper(cmd),
            eos::hardware::ReleaseResult::kAccepted);
  EXPECT_EQ(registry.Gripper()->ReadProprioception().force_n, 7.0);
}

TEST(RegistryTest, MissingKindIsNamed) {
  Registry registry = Registry::Adopt(ParseBinding(kBaseBinding, "inline.toml", "test"));
  const std::string error = ErrorOf([&] { registry.Find(DeviceKind::kCamera); });
  EXPECT_NE(error.find("camera"), std::string::npos);
}

TEST(RegistryTest, UnknownBackendNamesStringAndFile) {
  // Parse-time rejection names the selector and the file.
  const std::string parse_error = ErrorOf(
      [&] { ParseBinding("[[profile.test.device]]\nclass = \"vendor\"\n", "bad.toml", "test"); });
  EXPECT_NE(parse_error.find("unknown class 'vendor'"), std::string::npos);
  EXPECT_NE(parse_error.find("bad.toml"), std::string::npos);

  // The registry factory keeps the same contract for hand-built bindings.
  Binding hand_built;
  hand_built.path = "bad.toml";
  hand_built.devices.push_back(
      eos::runtime::hardware_runtime::DeviceBinding{.backend_type = "demo-nope"});
  const std::string error = ErrorOf([&] { Registry::Adopt(std::move(hand_built)); });
  EXPECT_NE(error.find("demo-nope"), std::string::npos);
  EXPECT_NE(error.find("bad.toml"), std::string::npos);
}

TEST(RegistryTest, UndersizedRateEnvelopeThrows) {
  const std::string undersized = R"toml(
[[profile.test.device]]
class = "arm"

[profile.test.device.backend]
type = "demo"

[profile.test.device.feeds.geometric]
max_rate_hz = 0.0

[profile.test.device.feeds.proprioception]
max_rate_hz = 5.0

[profile.test.device.feeds.sensor]
max_rate_hz = 0.0

[profile.test.device.feeds.health]
max_rate_hz = 10.0
)toml";
  const std::string error =
      ErrorOf([&] { Registry::Adopt(ParseBinding(undersized, "slow.toml", "test")); });
  EXPECT_NE(error.find("arm-0"), std::string::npos);
  EXPECT_NE(error.find("proprioception"), std::string::npos);
  EXPECT_NE(error.find("10"), std::string::npos);  // declared rate
  EXPECT_NE(error.find("5"), std::string::npos);   // envelope rate
}

TEST(RegistryTest, TickNoOpsWithoutIntegrators) {
  const char* kArmBinding = R"toml(
[[profile.test.device]]
class = "arm"

[profile.test.device.backend]
type = "demo"

[profile.test.device.feeds.geometric]
max_rate_hz = 0.0

[profile.test.device.feeds.proprioception]
max_rate_hz = 500.0

[profile.test.device.feeds.sensor]
max_rate_hz = 0.0

[profile.test.device.feeds.health]
max_rate_hz = 10.0
)toml";
  Registry registry = Registry::Adopt(ParseBinding(kArmBinding, "inline.toml", "test"));
  registry.Tick(0.1);  // default no-op: nothing to integrate, nothing to throw
  SUCCEED();
}

TEST(RegistryTest, HaltTripsOnlyDeviceHealth) {
  Registry registry = Registry::Load(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo");
  EXPECT_FALSE(registry.EstopLatched());

  const_cast<eos::hardware::Backend&>(registry.Find(DeviceKind::kBase).feeds()).Halt();
  EXPECT_FALSE(registry.EstopLatched());
  EXPECT_FALSE(registry.Find(DeviceKind::kBase).ReadHealth().alive);
  EXPECT_TRUE(registry.Find(DeviceKind::kCamera).ReadHealth().alive);
}

}  // namespace
