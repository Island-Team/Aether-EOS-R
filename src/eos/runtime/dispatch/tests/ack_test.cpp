#include "ack.h"
#include <algorithm>
using namespace dispatch_test;

TEST(PrimitiveAckTest, CorrelatesAcceptedAndRejectedCalls) {
  auto registry = DemoRobot();
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  const auto accepted =
      Exec(dispatcher, "base.posture", "base-0", {{"posture", "stand"}, {"command_id", "p-1"}});
  ExpectAccepted(accepted, "base.posture");
  EXPECT_EQ(accepted.at("command_id"), "p-1");
  const auto rejected =
      dispatcher.Execute("unknown.primitive", "1.0.0", "base-0", nlohmann::json::object());
  EXPECT_EQ(rejected.status, eos::runtime::dispatch::DispatchResult::Status::kNotFound);
  const auto wrong_version =
      dispatcher.Execute("base.posture", "2.0.0", "base-0", {{"posture", "stand"}});
  EXPECT_EQ(wrong_version.status, eos::runtime::dispatch::DispatchResult::Status::kNotFound);
  EXPECT_GT(accepted.at("sequence").get<int>(), 0);
}

TEST(PrimitiveAckTest, ExplicitTargetIsRequired) {
  auto registry = DemoRobot();
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  ExpectRejected(Exec(dispatcher, "base.posture", "", {{"posture", "stand"}}), "base.posture",
                 "rejected.invalid_target");
}

TEST(PrimitiveAckTest, NavigationGaitRemainsInDiscoveryWithoutNumericLimit) {
  std::ifstream input(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml");
  std::ostringstream source;
  source << input.rdbuf();
  std::string binding = source.str();
  const std::string old_identity = "name = \"base.navigate\"\nversion = \"1.0.0\"";
  const auto position = binding.find(old_identity);
  ASSERT_NE(position, std::string::npos);
  binding.replace(position, old_identity.size(),
                  "name = \"base.navigate\"\nversion = \"1.1.0\"");
  auto registry = eos::runtime::hardware_runtime::Registry::Adopt(
      eos::runtime::hardware_runtime::manager::BuildBackendCatalog(),
      eos::runtime::hardware_runtime::ParseBinding(
          binding, "gait-discovery.toml", "demo",
          eos::runtime::hardware_runtime::manager::BuildBackendCatalog()));
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  const auto discovered = dispatcher.Discover().at("primitives");
  const auto navigation = std::find_if(discovered.begin(), discovered.end(), [](const auto& item) {
    return item.at("name") == "base.navigate";
  });
  ASSERT_NE(navigation, discovered.end());
  EXPECT_TRUE(navigation->at("parameters").at("properties").contains("gait"));
  EXPECT_FALSE(navigation->at("limits").contains("gait"));
}

TEST(PrimitiveAckTest, NavigationAliasUsesConfiguredDiscoveryChoices) {
  std::ifstream input(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml");
  std::ostringstream source;
  source << input.rdbuf();
  std::string binding = source.str();
  const std::string old_identity = "name = \"base.navigate\"\nversion = \"1.0.0\"";
  const auto position = binding.find(old_identity);
  ASSERT_NE(position, std::string::npos);
  binding.replace(position, old_identity.size(),
                  "name = \"base.navigate\"\nversion = \"1.2.0\"\n"
                  "gait_aliases = { easy = \"walk\", steps = \"perceived_stairs\" }");
  auto registry = eos::runtime::hardware_runtime::Registry::Adopt(
      eos::runtime::hardware_runtime::manager::BuildBackendCatalog(),
      eos::runtime::hardware_runtime::ParseBinding(
          binding, "gait-alias-discovery.toml", "demo",
          eos::runtime::hardware_runtime::manager::BuildBackendCatalog()));
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  const auto discovered = dispatcher.Discover().at("primitives");
  const auto navigation = std::find_if(discovered.begin(), discovered.end(), [](const auto& item) {
    return item.at("name") == "base.navigate";
  });
  ASSERT_NE(navigation, discovered.end());
  EXPECT_EQ(navigation->at("parameters").at("properties").at("gait").at("enum"),
            nlohmann::json::array({"easy", "steps"}));
  const auto invalid = dispatcher.Execute(
      "base.navigate", "1.2.0", "base-0",
      {{"reference", "relative"}, {"x_m", 0.1}, {"y_m", 0.0}, {"yaw_rad", 0.0},
       {"gait", "unknown"}});
  ASSERT_EQ(invalid.status, eos::runtime::dispatch::DispatchResult::Status::kCompleted);
  EXPECT_FALSE(invalid.payload.at("accepted").get<bool>());
}
