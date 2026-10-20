#include "ack.h"
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
