#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "share/skills/native/common/assist_support.h"
#include "share/skills/native/register_host/name_parser.h"
#include "src/eos/lib/timer.h"
#include "src/eos/runtime/skills/builtin/registry.h"

namespace island::skills::native {
namespace {

using ::eos::runtime::skills::builtin::Registry;
using ::eos::runtime::skills::builtin::CapabilityHooks;
using ::eos::runtime::skills::builtin::Execution;
using ::eos::runtime::skills::ExecutionHooks;
using ::island::skills::assist::ParseSwitchRequest;
using ::island::skills::assist::HumanCondition;
using ::island::skills::assist::MatchesAction;
using ::island::skills::assist::MatchesCondition;
using ::island::skills::assist::MatchesPose;
using ::island::skills::assist::SwitchRequest;
using Json = nlohmann::json;

class AssistSkillsTest : public testing::Test {
 protected:
  eos::lib::ThreadTimerService timer_;
};

TEST_F(AssistSkillsTest, ParsesChineseSwitchRequestsConservatively) {
  EXPECT_EQ(ParseSwitchRequest("\u8bf7\u5e2e\u6211\u6253\u5f00\u7535\u5668"), SwitchRequest::kOn);
  EXPECT_EQ(ParseSwitchRequest("\u8bf7\u5173\u6389\u5b83"), SwitchRequest::kOff);
  EXPECT_EQ(ParseSwitchRequest("\u4e0d\u8981\u6253\u5f00"), SwitchRequest::kOff);
  EXPECT_EQ(ParseSwitchRequest("\u5148\u6253\u5f00\u518d\u5173\u95ed"), SwitchRequest::kAmbiguous);
  EXPECT_EQ(ParseSwitchRequest("\u968f\u4fbf"), SwitchRequest::kAmbiguous);
}

TEST_F(AssistSkillsTest, PrefersCanonicalActionAndPoseOverLegacyActivity) {
  EXPECT_TRUE(MatchesAction({{"attributes", {{"action", "waving"},
                                               {"activity", "fallen"}}}},
                            "waving", "waving"));
  EXPECT_FALSE(MatchesAction({{"attributes", {{"action", "none"},
                                                 {"activity", "waving"}}}},
                             "waving", "waving"));
  EXPECT_TRUE(MatchesAction({{"attributes", {{"activity", "waving"}}}},
                            "waving", "waving"));

  EXPECT_TRUE(MatchesCondition({{"attributes", {{"action", "fallen"}}}},
                               HumanCondition::kFallen));
  EXPECT_FALSE(MatchesCondition({{"attributes", {{"action", "none"},
                                                   {"activity", "fallen"}}}},
                                HumanCondition::kFallen));
  EXPECT_TRUE(MatchesCondition({{"attributes", {{"activity", "fallen"}}}},
                               HumanCondition::kFallen));

  EXPECT_TRUE(MatchesPose({{"attributes", {{"pose", "sit"},
                                              {"activity", "standing"}}}},
                          "sit", "sitting"));
  EXPECT_FALSE(MatchesCondition({{"attributes", {{"pose", "stand"},
                                                    {"activity", "sitting"}}}},
                                HumanCondition::kSitting));
  EXPECT_TRUE(MatchesCondition({{"attributes", {{"activity", "sitting"}}}},
                               HumanCondition::kSitting));
}

TEST_F(AssistSkillsTest, SignalReceiveFailureIsNotTreatedAsCancellation) {
  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<eos::runtime::skills::Signal> {
    throw std::runtime_error("signal transport failed");
  };
  const Json arguments = Json::object();
  Execution execution(timer_, arguments, "signal-failure", std::move(hooks));
  assist::ControlState control("assist_test");
  std::string error;

  auto task = assist::WaitUntilRunnable(execution, &control, &error);
  EXPECT_FALSE(task.Get());
  EXPECT_EQ(error, "capability call failed");
  EXPECT_FALSE(execution.cancelled());
}

TEST_F(AssistSkillsTest, AsrWaitRejectsStaleSequenceAndPropagatesReadFailure) {
  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) {
    return std::optional<eos::runtime::skills::Signal>{};
  };
  const Json arguments = Json::object();
  std::size_t reads = 0;
  CapabilityHooks capabilities;
  capabilities.read_resource = [&](const std::string&) {
    ++reads;
    if (reads == 1)
      return eos::runtime::skills::builtin::Result<Json>::Success(
          {{"sequence", 7}, {"text", "stale"}});
    return eos::runtime::skills::builtin::Result<Json>::Failure(
        {"read_failed", "ASR transport failed"});
  };
  Execution execution(timer_, arguments, "asr-freshness", std::move(hooks),
                      std::move(capabilities));
  assist::ControlState control("assist_test");
  Json recognized;
  std::string error;

  auto task = assist::WaitForAsrAfter(execution, &control, "asr", 7,
                                      assist::Clock::now() + std::chrono::seconds(1),
                                      &recognized, &error);
  EXPECT_FALSE(task.Get());
  EXPECT_TRUE(recognized.is_null());
  EXPECT_EQ(reads, 2u);
  EXPECT_EQ(error, "capability call failed");
}

TEST(RegisterHostTest, ExtractsIntroducedNames) {
  using register_host::ParseIntroducedName;
  EXPECT_EQ(ParseIntroducedName("我是张三。"), "张三");
  EXPECT_EQ(ParseIntroducedName("你好，我叫李小明，很高兴认识你"), "李小明");
  EXPECT_EQ(ParseIntroducedName("我的名字是 王芳"), "王芳");
  EXPECT_EQ(ParseIntroducedName("My name is Alice Smith."), "Alice Smith");
  EXPECT_EQ(ParseIntroducedName("I'M Bob!"), "Bob");
}

TEST(RegisterHostTest, RejectsSpeechWithoutAUsableName) {
  using register_host::ParseIntroducedName;
  EXPECT_FALSE(ParseIntroducedName("请自我介绍"));
  EXPECT_FALSE(ParseIntroducedName("我是"));
  EXPECT_FALSE(ParseIntroducedName("我是主人"));
  EXPECT_FALSE(ParseIntroducedName("My name is unknown"));
}

}  // namespace
}  // namespace island::skills::native
