/// Mode config tests: the committed etc/eos/eos.example.toml `[mode]` section resolves
/// the selected backend profile with operator control enabled. Missing
/// sections and keys fail loudly.
#include "mode.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using eos::runtime::hardware_runtime::ParseMode;

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

std::string ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open " + path);
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

TEST(ModeTest, CommittedModeSelectsDemo) {
  const auto config = ParseMode(ReadFile(std::string(EOS_SOURCE_ETC) + "/eos.example.toml"),
                                "eos.example.toml");
  ASSERT_EQ(config.bindings.size(), 1u);
  EXPECT_EQ(config.bindings[0].backend, "backends/demo.toml");
  EXPECT_EQ(config.bindings[0].profile, "demo");
  EXPECT_TRUE(config.expose_operator);
}

TEST(ModeTest, OptedInModeExposesOperator) {
  const char* config_text = R"toml(
[mode]
[[mode.binding]]
backend = "backends/demo.toml"
profile = "demo"

[mode.primitives]
expose_operator = true
)toml";
  const auto config = ParseMode(config_text, "inline.toml");
  ASSERT_EQ(config.bindings.size(), 1u);
  EXPECT_EQ(config.bindings[0].backend, "backends/demo.toml");
  EXPECT_EQ(config.bindings[0].profile, "demo");
  EXPECT_TRUE(config.expose_operator);
}

TEST(ModeTest, MissingProfileFails) {
  const std::string error = ErrorOf([&] { ParseMode("[mode]\n", "inline.toml"); });
  EXPECT_NE(error.find("missing required '[[mode.binding]]'"), std::string::npos);
}

TEST(ModeTest, NonStringProfileFails) {
  const std::string error = ErrorOf(
      [&] { ParseMode("[mode]\n[[mode.binding]]\nbackend = \"x\"\nprofile = 1\n", "eos.toml"); });
  EXPECT_NE(error.find("key 'profile' must be a string"), std::string::npos);
}

TEST(ModeTest, MissingModeTableFails) {
  const std::string error =
      ErrorOf([&] { ParseMode("[gateway]\nhost = \"127.0.0.1\"\n", "eos.toml"); });
  EXPECT_NE(error.find("missing required table '[mode]'"), std::string::npos);
}

TEST(ModeTest, MissingBackendFails) {
  const std::string error =
      ErrorOf([&] { ParseMode("[mode]\n[[mode.binding]]\nprofile = \"x\"\n", "eos.toml"); });
  EXPECT_NE(error.find("key 'backend' must be a string"), std::string::npos);
}

TEST(ModeTest, MultipleBindingsRequireSeparateProcesses) {
  const std::string error = ErrorOf([&] {
    ParseMode(
        "[mode]\n[[mode.binding]]\nbackend = \"a.toml\"\nprofile = \"a\"\n"
        "[[mode.binding]]\nbackend = \"b.toml\"\nprofile = \"b\"\n",
        "eos.toml");
  });
  EXPECT_NE(error.find("exactly one"), std::string::npos);
  EXPECT_NE(error.find("separate processes and ports"), std::string::npos);
}

TEST(ModeTest, UnknownPrimitiveKeyFails) {
  const std::string error = ErrorOf([&] {
    ParseMode(
        "[mode]\n[[mode.binding]]\nbackend = \"x\"\nprofile = \"y\"\n\n[mode.primitives]\nother = "
        "true\n",
        "eos.toml");
  });
  EXPECT_NE(error.find("unknown key 'other' in [mode.primitives]"), std::string::npos);
}

TEST(ModeTest, LegacyToolsTableFails) {
  const std::string error = ErrorOf([&] {
    ParseMode(
        "[mode]\n[[mode.binding]]\nbackend = \"x\"\nprofile = \"y\"\n\n[mode.tools]\n"
        "expose_operator = true\n",
        "eos.toml");
  });
  EXPECT_NE(error.find("unknown key 'tools' in [mode]"), std::string::npos);
}

}  // namespace
