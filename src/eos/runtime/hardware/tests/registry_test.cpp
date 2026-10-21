/// Registry admission tests use a synthetic backend through the public catalog seam.
#include "registry.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "test_backend.h"

namespace {

using eos::hardware::DeviceKind;
using eos::runtime::hardware_runtime::Binding;
using eos::runtime::hardware_runtime::Device;
using eos::runtime::hardware_runtime::ParseBinding;
using eos::runtime::hardware_runtime::Registry;
using eos::runtime::hardware_runtime::test::Catalog;

static_assert(
    std::is_same_v<decltype(std::declval<const Device&>().feeds()), const eos::hardware::Backend&>);
static_assert(
    std::is_same_v<decltype(std::declval<Registry&>().Find(DeviceKind::kBase)), const Device&>);

constexpr const char* kRuntimeId = "018f22d2-82ab-7def-9234-56789abcdef0";
constexpr const char* kBinding = R"toml(
[profile.test.robot]
robot_id = "robot-a"
robot_name = "test robot"

[[profile.test.device]]
class = "base"
[profile.test.device.backend]
type = "test"
[profile.test.device.feeds.proprioception]
max_rate_hz = 100.0
[profile.test.device.feeds.health]
max_rate_hz = 10.0

[[profile.test.device]]
class = "arm"
[profile.test.device.backend]
type = "test"
[profile.test.device.feeds.proprioception]
max_rate_hz = 500.0
[profile.test.device.feeds.health]
max_rate_hz = 10.0
)toml";

template <typename Body>
std::string ErrorOf(Body body) {
  try {
    body();
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return "";
}

Binding Parse(const std::string& text = kBinding, const std::string& path = "test.toml") {
  return ParseBinding(text, path, "test", Catalog());
}

TEST(RegistryTest, AdmitsSyntheticBackendsThroughCatalog) {
  Registry registry = Registry::Adopt(Catalog(), Parse(), kRuntimeId);
  ASSERT_EQ(registry.devices().size(), 2u);
  EXPECT_EQ(registry.robot().id, kRuntimeId);
  EXPECT_EQ(registry.Find(DeviceKind::kBase).identity().device_id, "base-0");
  EXPECT_EQ(registry.Find(DeviceKind::kManipulator).identity().device_id, "arm-0");
  EXPECT_EQ(registry.FindDevice("base-0"), &registry.Find(DeviceKind::kBase));
  EXPECT_EQ(registry.FindDevice("unknown"), nullptr);
  EXPECT_EQ(&registry.Find(DeviceKind::kBase, "default"), &registry.Find(DeviceKind::kBase));
  for (const Device& device : registry.devices()) {
    EXPECT_EQ(device.robot_id(), kRuntimeId);
    EXPECT_TRUE(device.ReadHealth().alive);
    EXPECT_TRUE(device.ReadHealth().admitted);
  }
}

TEST(RegistryTest, UsesConfiguredRobotIdentityWhenRuntimeIdIsOmitted) {
  Registry registry = Registry::Adopt(Catalog(), Parse());
  EXPECT_EQ(registry.robot().id, "robot-a");
  EXPECT_EQ(registry.robot().name, "test robot");
}

TEST(RegistryTest, RejectsMultipleBindingsPerProcess) {
  const std::string error =
      ErrorOf([&] { Registry::Adopt(Catalog(), std::vector<Binding>{Parse(), Parse()}); });
  EXPECT_NE(error.find("exactly one hardware binding"), std::string::npos);
}

TEST(RegistryTest, MissingKindAndPartAreNamed) {
  Registry registry = Registry::Adopt(Catalog(), Parse());
  EXPECT_NE(ErrorOf([&] { registry.Find(DeviceKind::kCamera); }).find("camera"), std::string::npos);
  EXPECT_NE(ErrorOf([&] { registry.Find(DeviceKind::kBase, "left"); }).find("left"),
            std::string::npos);
}

TEST(RegistryTest, ReportsKnownButUncompiledBackendAtAdmission) {
  const std::string error = ErrorOf([&] { Registry::Adopt(Catalog(false), Parse()); });
  EXPECT_NE(error.find("test"), std::string::npos);
  EXPECT_NE(error.find("not compiled"), std::string::npos);
}

TEST(RegistryTest, UnknownBackendNamesSelectorAndFile) {
  Binding hand_built = Parse();
  hand_built.path = "bad.toml";
  hand_built.devices.front().backend_type = "missing";
  const std::string error = ErrorOf([&] { Registry::Adopt(Catalog(), hand_built); });
  EXPECT_NE(error.find("missing"), std::string::npos);
  EXPECT_NE(error.find("bad.toml"), std::string::npos);
}

TEST(RegistryTest, RejectsBackendIdentityMismatch) {
  const std::string error = ErrorOf([&] { Registry::Adopt(Catalog(true, false, true), Parse()); });
  EXPECT_NE(error.find("inconsistent identity"), std::string::npos);
  EXPECT_NE(error.find("base-0"), std::string::npos);
}

TEST(RegistryTest, UndersizedRateEnvelopeFailsAdmission) {
  Binding binding = Parse();
  binding.devices.front().envelope.proprioception.max_rate_hz = 5.0;
  const std::string error = ErrorOf([&] { Registry::Adopt(Catalog(), binding); });
  EXPECT_NE(error.find("base-0"), std::string::npos);
  EXPECT_NE(error.find("proprioception"), std::string::npos);
  EXPECT_NE(error.find("10"), std::string::npos);
  EXPECT_NE(error.find("5"), std::string::npos);
}

TEST(RegistryTest, TickAndHaltUseGeneralBackendInterface) {
  Registry registry = Registry::Adopt(Catalog(), Parse());
  EXPECT_FALSE(registry.EstopLatched());
  EXPECT_NO_THROW(registry.Tick(0.1));
  const_cast<eos::hardware::Backend&>(registry.Find(DeviceKind::kBase).feeds()).Halt();
  EXPECT_FALSE(registry.EstopLatched());
  EXPECT_FALSE(registry.Find(DeviceKind::kBase).ReadHealth().alive);
  EXPECT_TRUE(registry.Find(DeviceKind::kManipulator).ReadHealth().alive);
}

}  // namespace
