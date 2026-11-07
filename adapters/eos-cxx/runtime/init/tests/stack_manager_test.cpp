#include "adapters/eos-cxx/runtime/init/stack_manager.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "adapters/eos-cxx/runtime/hardware/tests/test_backend.h"

namespace eos::runtime::init {
namespace {

class StackManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<unsigned> sequence{0};
    root_ = std::filesystem::temp_directory_path() /
            ("eos-stack-test-" + std::to_string(++sequence));
    std::filesystem::create_directories(root_ / "runtime");
    std::filesystem::create_directories(root_ / "ext/simulator");
    std::filesystem::create_directories(root_ / "eos/backends");
    Write("eos/eos.toml", "[mode]\n[[mode.binding]]\nbackend = 'backends/demo.toml'\nprofile = 'demo'\n");
    Write("eos/backends/demo.toml", "version = 1\n");
    Write("runtime/stack.toml",
          "version = 1\neos_config = '../eos/eos.toml'\n"
          "modules = ['../ext/simulator/module.toml']\n"
          "[[service]]\nname = 'eosd'\nrole = 'managed'\ncommand = ['eosd']\n");
    Write("ext/simulator/module.toml",
          "version = 1\n[module]\nid = 'simulator'\nenabled = true\n"
          "[[service]]\nname = 'simulator'\nrole = 'managed'\ncommand = ['simulator']\n"
          "[[binding]]\nbackend = 'backends/demo.toml'\nprofile = 'demo'\n"
          "requires = ['simulator']\neos_after = ['simulator']\n");
  }

  void TearDown() override { std::filesystem::remove_all(root_); }

  void Write(const std::string& path, const std::string& content) {
    std::ofstream(root_ / path) << content;
  }

  void Validate() {
    hardware_runtime::ModeConfig mode;
    mode.bindings.push_back({"backends/demo.toml", "demo"});
    ValidateRuntimeStack(root_ / "runtime/stack.toml", root_ / "eos/eos.toml", mode);
  }

  std::filesystem::path root_;
};

TEST_F(StackManagerTest, AdmitsExplicitEnabledModuleBinding) { EXPECT_NO_THROW(Validate()); }

TEST_F(StackManagerTest, RejectsUnlistedModule) {
  Write("runtime/stack.toml", "version = 1\neos_config = '../eos/eos.toml'\n"
                              "modules = []\n[[service]]\nname = 'eosd'\n");
  EXPECT_THROW(Validate(), std::runtime_error);
}

TEST_F(StackManagerTest, RejectsDisabledRequiredService) {
  Write("ext/simulator/module.toml",
        "version = 1\n[module]\nid = 'simulator'\n"
        "[[service]]\nname = 'simulator'\nenabled = false\n"
        "[[binding]]\nbackend = 'backends/demo.toml'\nprofile = 'demo'\n"
        "requires = ['simulator']\n");
  EXPECT_THROW(Validate(), std::runtime_error);
}

TEST_F(StackManagerTest, RejectsDifferentEosConfig) {
  Write("eos/other.toml", "[mode]\n");
  EXPECT_THROW(ValidateRuntimeStack(root_ / "runtime/stack.toml", root_ / "eos/other.toml",
                                    hardware_runtime::ModeConfig{}), std::runtime_error);
}

TEST_F(StackManagerTest, RejectsDuplicateModuleReference) {
  Write("runtime/stack.toml",
        "version = 1\neos_config = '../eos/eos.toml'\n"
        "modules = ['../ext/simulator/module.toml', '../ext/simulator/module.toml']\n");
  EXPECT_THROW(Validate(), std::runtime_error);
}

TEST_F(StackManagerTest, PackageModuleRequiresSelectedRelease) {
  std::filesystem::create_directories(root_ / "ext/island--simulator");
  Write("runtime/stack.toml",
        "version = 1\neos_config = '../eos/eos.toml'\npackage_root = '../packages'\n"
        "modules = ['../ext/island--simulator/module.toml']\n"
        "[[service]]\nname = 'eosd'\n");
  Write("ext/island--simulator/module.toml",
        "version = 1\nskill_directory = '.'\n[module]\nid = 'island--simulator'\n"
        "package = 'island/simulator'\nversion = '1.2.3'\n"
        "[[binding]]\nbackend = 'backends/demo.toml'\nprofile = 'demo'\n");
  EXPECT_THROW(Validate(), std::runtime_error);
  std::filesystem::create_directories(root_ / "packages/installed/island/simulator/1.2.3");
  Write("packages/installed/island/simulator/1.2.3/release.json", "{}");
  Write("packages/isos.lock", "version = 1\n[[package]]\nid = 'island/simulator'\n"
                              "version = '1.2.3'\n"
                              "digest = 'sha256:b551b00031abf2ec71ed10e5bfe59a9dc388ebf18ed0283be7987cf49e372d27'\n");
  hardware_runtime::ModeConfig package_mode;
  package_mode.bindings.push_back({"backends/demo.toml", "demo"});
  const auto selected = ValidateRuntimeStack(root_ / "runtime/stack.toml",
                                              root_ / "eos/eos.toml", package_mode);
  ASSERT_EQ(selected.skill_providers.size(), 1u);
  EXPECT_EQ(selected.skill_providers[0].module_id, "island--simulator");
  EXPECT_EQ(selected.skill_providers[0].directory,
            std::filesystem::canonical(root_ / "packages/installed/island/simulator/1.2.3"));
  Write("packages/installed/island/simulator/1.2.3/release.json", "{\"tampered\":true}");
  EXPECT_THROW(Validate(), std::runtime_error);
  Write("packages/isos.lock", "version = 1\n[[package]]\nid = 'island/simulator'\n"
                              "version = '1.2.4'\n"
                              "digest = 'sha256:b551b00031abf2ec71ed10e5bfe59a9dc388ebf18ed0283be7987cf49e372d27'\n");
  EXPECT_THROW(Validate(), std::runtime_error);
}

TEST_F(StackManagerTest, ParsesOnlyEnabledSkillCapabilitySlots) {
  Write("ext/simulator/module.toml",
        "version = 1\n[module]\nid = 'simulator'\nenabled = true\n"
        "[[service]]\nname = 'simulator'\n"
        "[[binding]]\nbackend = 'backends/demo.toml'\nprofile = 'demo'\n"
        "[[skill_capability]]\nskill = 'sample.posture'\n"
        "skill_version = '1.0.0'\nslot = 'target'\n"
        "argument_pointer = '/target/part'\nrobot_id = 'robot-a'\n"
        "device_class = 'base'\nprimitive = 'base.posture'\n"
        "primitive_version = '1.0.0'\n");
  hardware_runtime::ModeConfig mode;
  mode.bindings.push_back({"backends/demo.toml", "demo"});
  const auto stack = ValidateRuntimeStack(root_ / "runtime/stack.toml",
                                          root_ / "eos/eos.toml", mode);
  ASSERT_EQ(stack.skill_capabilities.size(), 1u);
  EXPECT_EQ(stack.skill_capabilities[0].argument_pointer, "/target/part");
  Write("runtime/stack.toml",
        "version = 1\neos_config = '../eos/eos.toml'\n"
        "modules = ['../ext/simulator/module.toml']\n"
        "[[service]]\nname = 'eosd'\n"
        "[[binding]]\nbackend = 'backends/demo.toml'\nprofile = 'demo'\n");
  Write("ext/simulator/module.toml",
        "version = 1\n[module]\nid = 'simulator'\nenabled = false\n"
        "package = 'invalid/package'\nversion = '0.0.0'\n"
        "[[skill_capability]]\nskill = 'bad'\n");
  const auto disabled = ValidateRuntimeStack(root_ / "runtime/stack.toml",
                                             root_ / "eos/eos.toml", mode);
  EXPECT_TRUE(disabled.skill_capabilities.empty());
}

TEST(StackManagerSkillGrantsTest, ResolvesArgumentPartIntoCanonicalDeviceAndRejectsOtherTarget) {
  const auto binding = hardware_runtime::ParseBinding(R"toml(
[profile.test.robot]
robot_id = "robot-a"
[[profile.test.device]]
class = "base"
part_id = "front"
[[profile.test.device.primitive]]
name = "base.posture"
version = "1.0.0"
description = "Test posture."
[profile.test.device.backend]
type = "test"
[profile.test.device.feeds.proprioception]
max_rate_hz = 10.0
[profile.test.device.feeds.health]
max_rate_hz = 10.0
)toml", "capability-test.toml", "test", hardware_runtime::test::Catalog());
  const auto registry = hardware_runtime::Registry::Adopt(hardware_runtime::test::Catalog(), binding);
  skills::Metadata metadata;
  metadata.name = "sample.posture";
  metadata.version = "1.0.0";
  metadata.primitives.push_back({"base.posture", "1.0.0"});
  const SkillCapability capability{"sample.posture", "1.0.0", "target", "/target/part",
                                   "robot-a", "base", "base.posture", "1.0.0"};
  const auto grants = ResolveSkillGrants({capability}, registry, metadata,
                                         {{"target", {{"part", "front"}}}}, "exec-1");
  ASSERT_EQ(grants.size(), 1u);
  EXPECT_EQ(grants[0].device_id, "base-0");
  EXPECT_EQ(grants[0].selector, "front");
  EXPECT_THROW(ResolveSkillGrants({capability}, registry, metadata,
                                 {{"target", {{"part", "rear"}}}}, "exec-1"),
               std::invalid_argument);
  EXPECT_THROW(ResolveSkillGrants({capability}, registry, metadata,
                                 {{"target", {{"part", "base-0"}}}}, "exec-1"),
               std::invalid_argument);
}

TEST_F(StackManagerTest, RejectsDependencyCycleIntroducedByBinding) {
  Write("ext/simulator/module.toml",
        "version = 1\n[module]\nid = 'simulator'\n"
        "[[service]]\nname = 'simulator'\nafter = ['eosd']\n"
        "[[binding]]\nbackend = 'backends/demo.toml'\nprofile = 'demo'\n"
        "requires = ['simulator']\neos_after = ['simulator']\n");
  EXPECT_THROW(Validate(), std::runtime_error);
}

}  // namespace
}  // namespace eos::runtime::init
