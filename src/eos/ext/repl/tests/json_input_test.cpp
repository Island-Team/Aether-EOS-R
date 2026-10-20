#include "src/eos/ext/repl/json_input.h"

#include <gtest/gtest.h>

#include <deque>
#include <vector>

namespace eos::ext::repl {
namespace {

TEST(JsonInputTest, RetainsInvalidInputUntilAnObjectIsSubmitted) {
  std::deque<std::string> submissions{"[1]", "{bad", R"({"speed":0.2})"};
  std::vector<std::string> preloads;
  std::vector<std::string> errors;
  const auto result = ReadJsonObject(
      [&](const std::string& preload, std::string* submitted) {
        preloads.push_back(preload);
        *submitted = submissions.front();
        submissions.pop_front();
        return true;
      },
      [&](const std::string& error) { errors.push_back(error); });
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->at("speed"), 0.2);
  EXPECT_EQ(preloads, (std::vector<std::string>{"{}", "[1]", "{bad"}));
  ASSERT_EQ(errors.size(), 2u);
  EXPECT_NE(errors[0].find("object"), std::string::npos);
  EXPECT_NE(errors[1].find("invalid JSON"), std::string::npos);
}

TEST(JsonInputTest, CancellationReturnsNoValue) {
  const auto result = ReadJsonObject([](const std::string&, std::string*) { return false; },
                                     [](const std::string&) {});
  EXPECT_FALSE(result.has_value());
}

}  // namespace
}  // namespace eos::ext::repl
