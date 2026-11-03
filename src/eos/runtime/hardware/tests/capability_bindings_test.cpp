#include "manager/capability_bindings.h"

#include <gtest/gtest.h>

#include <stdexcept>

#include "test_backend.h"

namespace {

using eos::runtime::hardware_runtime::ParseBinding;
using eos::runtime::hardware_runtime::Registry;
using eos::runtime::hardware_runtime::manager::CapabilitySlot;
using eos::runtime::hardware_runtime::manager::ResolveCapabilitySlots;

Registry TestRegistry() {
  const auto binding = ParseBinding(R"toml(
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
)toml", "capability-test.toml", "test", eos::runtime::hardware_runtime::test::Catalog());
  return Registry::Adopt(eos::runtime::hardware_runtime::test::Catalog(), binding);
}

TEST(CapabilityBindingsTest, ResolvesStablePartAndExactAdmittedPrimitive) {
  const Registry registry = TestRegistry();
  const auto resolved = ResolveCapabilitySlots(
      registry, {CapabilitySlot{"posture", "robot-a", "base", "front", "base.posture", "1.0.0"}});
  ASSERT_EQ(resolved.size(), 1u);
  EXPECT_EQ(resolved[0].device_id, "base-0");
  EXPECT_EQ(resolved[0].primitive, "base.posture");
}

TEST(CapabilityBindingsTest, RejectsWrongRobotPartPrimitiveAndDuplicateSlot) {
  const Registry registry = TestRegistry();
  CapabilitySlot slot{"posture", "robot-a", "base", "front", "base.posture", "1.0.0"};
  slot.robot_id = "robot-b";
  EXPECT_THROW(ResolveCapabilitySlots(registry, {slot}), std::invalid_argument);
  slot.robot_id = "robot-a";
  slot.part_id = "rear";
  EXPECT_THROW(ResolveCapabilitySlots(registry, {slot}), std::invalid_argument);
  slot.part_id = "front";
  slot.primitive = "base.navigate";
  EXPECT_THROW(ResolveCapabilitySlots(registry, {slot}), std::invalid_argument);
  slot.primitive = "base.posture";
  EXPECT_THROW(ResolveCapabilitySlots(registry, {slot, slot}), std::invalid_argument);
}

}  // namespace
