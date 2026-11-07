#include "adapters/eos-cxx/runtime/kernel/broker.h"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>

#include "adapters/eos-cxx/runtime/hardware/binding.h"
#include "adapters/eos-cxx/runtime/hardware/tests/test_backend.h"
#include "adapters/eos-cxx/runtime/init/schemas.h"

namespace {
using eos::runtime::dispatch::DispatchResult;
using eos::runtime::dispatch::Dispatcher;
using eos::runtime::hardware_runtime::ParseBinding;
using eos::runtime::hardware_runtime::Registry;
using eos::runtime::hardware_runtime::manager::CapabilitySlot;
using eos::runtime::kernel::Broker;
using eos::runtime::kernel::Catalog;

Registry TestRegistry() {
  const auto catalog = eos::runtime::hardware_runtime::test::Catalog();
  auto binding = ParseBinding(R"toml(
[profile.test.robot]
robot_id = "robot/a"
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
)toml", "kernel-test.toml", "test", catalog);
  return Registry::Adopt(catalog, std::move(binding));
}

TEST(KernelCatalogTest, EnumeratesCanonicalHardwareAndAliases) {
  auto registry = TestRegistry();
  Catalog catalog(registry, {CapabilitySlot{"posture", "robot/a", "base", "front",
                                            "base.posture", "1.0.0"}});
  const auto name = Catalog::HardwareName("robot/a", "base", "front", "base.posture", "1.0.0");
  EXPECT_EQ(name, "hardware/robot%2Fa/base/front/base.posture@1.0.0");
  ASSERT_EQ(catalog.List().size(), 5u);
  ASSERT_NE(catalog.Find(name), nullptr);
  ASSERT_NE(catalog.Find("posture"), nullptr);
  EXPECT_EQ(catalog.FindHardware("base.posture", "1.0.0", "base-0")->name, name);
  EXPECT_EQ(catalog.FindHardware("base.posture", "2.0.0", "base-0"), nullptr);
  EXPECT_EQ(catalog.FindHardware("base.posture", "1.0.0", "wrong"), nullptr);
  EXPECT_TRUE(catalog.Available("posture"));
  EXPECT_NE(catalog.FindHardware("safety.estop", "1.0.0", ""), nullptr);
  EXPECT_NE(catalog.FindHardware("robot.stop", "1.0.0", ""), nullptr);
  EXPECT_NE(catalog.FindHardware("motion.stop", "1.0.0", "base-0"), nullptr);
  EXPECT_FALSE(catalog.Available("missing"));
}

TEST(KernelCatalogTest, RejectsMissingAndReservedAliases) {
  auto registry = TestRegistry();
  EXPECT_THROW(Catalog(registry, {CapabilitySlot{"wrong", "robot/a", "base", "rear",
                                                       "base.posture", "1.0.0"}}),
               std::invalid_argument);
  const auto canonical = Catalog::HardwareName("robot/a", "base", "front", "base.posture",
                                                "1.0.0");
  EXPECT_THROW(Catalog(registry, {CapabilitySlot{canonical, "robot/a", "base", "front",
                                                       "base.posture", "1.0.0"}}),
               std::invalid_argument);
}

TEST(KernelBrokerTest, RequiresExactLiveGrantAndForwardsOneIdentifiedCommand) {
  auto registry = TestRegistry();
  Catalog catalog(registry, {});
  eos::runtime::init::SchemaStore schemas(std::string(EOS_CONTRACTS_ROOT) + "/schemas/eos");
  Dispatcher dispatcher(registry, schemas);
  Broker broker(catalog, dispatcher);
  const auto name = Catalog::HardwareName("robot/a", "base", "front", "base.posture", "1.0.0");
  auto args = nlohmann::json{{"posture", "stand"}};
  EXPECT_EQ(broker.Execute(name, "missing", "exec", 1, args, "kernel-1").status,
            DispatchResult::Status::kFailedPrecondition);
  EXPECT_EQ(dispatcher.GetCommand("kernel-1").status, DispatchResult::Status::kNotFound);

  const auto token = broker.Grant(name, "exec", 1,
                                  std::chrono::steady_clock::now() + std::chrono::minutes(1));
  EXPECT_EQ(broker.Execute(name, token, "other", 1, args, "kernel-1").status,
            DispatchResult::Status::kFailedPrecondition);
  EXPECT_EQ(broker.Execute(name, token, "exec", 2, args, "kernel-1").status,
            DispatchResult::Status::kFailedPrecondition);
  EXPECT_EQ(dispatcher.GetCommand("kernel-1").status, DispatchResult::Status::kNotFound);

  const auto first = broker.Execute(name, token, "exec", 1, args, "kernel-1");
  EXPECT_EQ(first.status, DispatchResult::Status::kCompleted);
  EXPECT_EQ(dispatcher.GetCommand("kernel-1").status, DispatchResult::Status::kCompleted);
  const auto replay = broker.Execute(name, token, "exec", 1, args, "kernel-1");
  EXPECT_EQ(replay.status, DispatchResult::Status::kCompleted);
  EXPECT_EQ(replay.payload, first.payload);
  broker.RevokeExecution("exec");
  EXPECT_EQ(broker.Execute(name, token, "exec", 1, args, "kernel-2").status,
            DispatchResult::Status::kFailedPrecondition);
  EXPECT_EQ(dispatcher.GetCommand("kernel-2").status, DispatchResult::Status::kNotFound);
}

TEST(KernelBrokerTest, StartTokenIsBoundToOneCatalogName) {
  auto registry = TestRegistry();
  Catalog catalog(registry, {CapabilitySlot{"posture.alias", "robot/a", "base", "front",
                                            "base.posture", "1.0.0"}});
  eos::runtime::init::SchemaStore schemas(std::string(EOS_CONTRACTS_ROOT) + "/schemas/eos");
  Dispatcher dispatcher(registry, schemas);
  Broker broker(catalog, dispatcher);
  const auto canonical = Catalog::HardwareName("robot/a", "base", "front", "base.posture",
                                                "1.0.0");
  const auto token = broker.Grant("posture.alias", "exec", 1,
                                  std::chrono::steady_clock::now() + std::chrono::minutes(1));
  const auto args = nlohmann::json{{"posture", "stand"}};
  EXPECT_EQ(broker.Start(canonical, token, "exec", 1, args, "wrong-alias").status,
            DispatchResult::Status::kFailedPrecondition);
  EXPECT_EQ(broker.Get("wrong-alias").status, DispatchResult::Status::kNotFound);
  const auto started = broker.Start("posture.alias", token, "exec", 1, args, "right-alias");
  EXPECT_TRUE(started.status == DispatchResult::Status::kPending ||
              started.status == DispatchResult::Status::kCompleted);
  broker.RevokeExecution("exec");
  EXPECT_EQ(broker.Start("posture.alias", token, "exec", 1, args, "revoked").status,
            DispatchResult::Status::kFailedPrecondition);
}

TEST(KernelBrokerTest, TrustedCapabilitySubmitUsesDispatcherCommandLifecycle) {
  auto registry = TestRegistry();
  Catalog catalog(registry, {});
  eos::runtime::init::SchemaStore schemas(std::string(EOS_CONTRACTS_ROOT) + "/schemas/eos");
  Dispatcher dispatcher(registry, schemas);
  Broker broker(catalog, dispatcher);
  const auto name = Catalog::HardwareName("robot/a", "base", "front", "base.posture", "1.0.0");
  const auto arguments = nlohmann::json{{"posture", "stand"}};
  EXPECT_EQ(broker.StartTrustedCapability("missing", arguments, "missing-op").status,
            DispatchResult::Status::kNotFound);
  EXPECT_EQ(dispatcher.GetCommand("missing-op").status, DispatchResult::Status::kNotFound);
  const auto started = broker.StartTrustedCapability(name, arguments, "known-op");
  EXPECT_TRUE(started.status == DispatchResult::Status::kPending ||
              started.status == DispatchResult::Status::kCompleted);
  const auto replay = broker.StartTrustedCapability(name, arguments, "known-op");
  EXPECT_TRUE(replay.status == DispatchResult::Status::kPending ||
              replay.status == DispatchResult::Status::kCompleted);
  EXPECT_NE(dispatcher.GetCommand("known-op").status, DispatchResult::Status::kNotFound);
}

TEST(KernelBrokerTest, TrustedCapabilityExecutionUsesDispatcherTerminalAck) {
  auto registry = TestRegistry();
  Catalog catalog(registry, {});
  eos::runtime::init::SchemaStore schemas(std::string(EOS_CONTRACTS_ROOT) + "/schemas/eos");
  Dispatcher dispatcher(registry, schemas);
  Broker broker(catalog, dispatcher);
  const auto name = Catalog::HardwareName("robot/a", "base", "front", "base.posture", "1.0.0");
  const auto result = broker.ExecuteTrustedCapability(name, {{"posture", "stand"}},
                                                       "workflow-capability");
  ASSERT_EQ(result.status, DispatchResult::Status::kCompleted);
  EXPECT_EQ(result.payload.at("command_id"), "workflow-capability");
  EXPECT_EQ(broker.ExecuteTrustedCapability("missing", nlohmann::json::object(),
                                            "workflow-missing").status,
            DispatchResult::Status::kNotFound);
}

TEST(KernelBrokerTest, TrustedEntryPreservesDispatchValidation) {
  auto registry = TestRegistry();
  Catalog catalog(registry, {});
  eos::runtime::init::SchemaStore schemas(std::string(EOS_CONTRACTS_ROOT) + "/schemas/eos");
  Dispatcher dispatcher(registry, schemas);
  Broker broker(catalog, dispatcher);
  EXPECT_EQ(broker.ExecuteTrustedHardware("base.posture", "2.0.0", "base-0",
                                           {{"posture", "stand"}}, "kernel-3").status,
            DispatchResult::Status::kNotFound);
  const auto wrong_target = broker.ExecuteTrustedHardware("base.posture", "1.0.0", "wrong",
                                                            {{"posture", "stand"}}, "kernel-4");
  ASSERT_EQ(wrong_target.status, DispatchResult::Status::kCompleted);
  EXPECT_EQ(wrong_target.payload.at("reason_code"), "rejected.invalid_target");
  EXPECT_EQ(broker.ExecuteTrustedHardware("base.posture", "", "base-0",
                                           {{"posture", "stand"}}, "kernel-5").status,
            DispatchResult::Status::kInvalidArgument);
  EXPECT_EQ(broker.StartTrustedHardware("", "1.0.0", "base-0",
                                         {{"posture", "stand"}}, "kernel-6").status,
            DispatchResult::Status::kInvalidArgument);
  EXPECT_EQ(dispatcher.GetCommand("kernel-3").status, DispatchResult::Status::kNotFound);
  EXPECT_EQ(dispatcher.GetCommand("kernel-4").status, DispatchResult::Status::kCompleted);
}

TEST(KernelBrokerTest, ProcessWideEstopRemainsAvailableAfterDeviceFailure) {
  auto registry = TestRegistry();
  Catalog catalog(registry, {});
  eos::runtime::init::SchemaStore schemas(std::string(EOS_CONTRACTS_ROOT) + "/schemas/eos");
  Dispatcher dispatcher(registry, schemas);
  Broker broker(catalog, dispatcher);
  EXPECT_THROW(broker.Grant("runtime/robot%2Fa/safety.estop@1.0.0", "exec", 1,
                            std::chrono::steady_clock::now() + std::chrono::minutes(1)),
               std::invalid_argument);
  const auto first = broker.ExecuteTrustedHardware("safety.estop", "1.0.0", "",
                                                    nlohmann::json::object(), "estop-1");
  EXPECT_EQ(first.status, DispatchResult::Status::kCompleted);
  EXPECT_TRUE(registry.EstopLatched());
  EXPECT_TRUE(catalog.Available("runtime/robot%2Fa/safety.estop@1.0.0"));
  const auto invalid = broker.ExecuteTrustedHardware("safety.estop", "1.0.0", "base-0",
                                                      nlohmann::json::object(), "estop-invalid");
  EXPECT_EQ(invalid.status, DispatchResult::Status::kCompleted);
  EXPECT_FALSE(invalid.payload.at("accepted").get<bool>());
  const auto replay = broker.ExecuteTrustedHardware("safety.estop", "1.0.0", "",
                                                     nlohmann::json::object(), "estop-1");
  EXPECT_EQ(replay.payload, first.payload);
}

TEST(KernelBrokerTest, DispatcherOwnsPostEstopRejectionAndCommandReplay) {
  auto registry = TestRegistry();
  Catalog catalog(registry, {});
  eos::runtime::init::SchemaStore schemas(std::string(EOS_CONTRACTS_ROOT) + "/schemas/eos");
  Dispatcher dispatcher(registry, schemas);
  Broker broker(catalog, dispatcher);
  const auto name = Catalog::HardwareName("robot/a", "base", "front", "base.posture", "1.0.0");
  const auto token = broker.Grant(name, "exec", 1,
                                  std::chrono::steady_clock::now() + std::chrono::minutes(1));
  const auto args = nlohmann::json{{"posture", "stand"}};
  const auto first = broker.Execute(name, token, "exec", 1, args, "before-estop");
  ASSERT_EQ(first.status, DispatchResult::Status::kCompleted);
  ASSERT_EQ(broker.ExecuteTrustedHardware("safety.estop", "1.0.0", "",
                                           nlohmann::json::object(), "estop-2").status,
            DispatchResult::Status::kCompleted);
  EXPECT_FALSE(catalog.Available(name));
  const auto replay = broker.Execute(name, token, "exec", 1, args, "before-estop");
  EXPECT_EQ(replay.payload, first.payload);
  const auto denied = broker.ExecuteTrustedHardware("base.posture", "1.0.0", "base-0", args,
                                                    "after-estop");
  ASSERT_EQ(denied.status, DispatchResult::Status::kCompleted);
  EXPECT_FALSE(denied.payload.at("accepted").get<bool>());
  EXPECT_EQ(denied.payload.at("reason_code"), "rejected.estop");
}
}  // namespace
