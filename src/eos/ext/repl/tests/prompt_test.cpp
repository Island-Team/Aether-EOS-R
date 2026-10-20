#include "src/eos/ext/repl/prompt.h"

#include <gtest/gtest.h>

namespace eos::ext::repl {
namespace {

TEST(PromptTest, UsesDiscoveredRobotNameRatherThanRuntimeId) {
  EXPECT_EQ(PromptIdentity(
                nlohmann::json{{"robot", {{"id", "runtime-uuid"}, {"name", "warehouse-7"}}}}),
            "warehouse-7");
  EXPECT_EQ(PromptIdentity(nlohmann::json{{"robot", {{"id", "runtime-uuid"}}}}), "offline");
  EXPECT_EQ(PromptIdentity(nlohmann::json::object()), "offline");
}

}  // namespace
}  // namespace eos::ext::repl
