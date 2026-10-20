#include "src/eos/ext/repl/history.h"

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace eos::ext::repl {
namespace {

class HistoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ =
        std::filesystem::temp_directory_path() /
        ("eos-repl-history-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
    old_xdg_ = std::getenv("XDG_STATE_HOME") ? std::getenv("XDG_STATE_HOME") : "";
    old_home_ = std::getenv("HOME") ? std::getenv("HOME") : "";
  }
  void TearDown() override {
    if (old_xdg_.empty())
      unsetenv("XDG_STATE_HOME");
    else
      setenv("XDG_STATE_HOME", old_xdg_.c_str(), 1);
    if (old_home_.empty())
      unsetenv("HOME");
    else
      setenv("HOME", old_home_.c_str(), 1);
    std::filesystem::remove_all(root_);
  }

  std::filesystem::path root_;
  std::string old_xdg_;
  std::string old_home_;
};

TEST_F(HistoryTest, ResolvesXdgThenHomeFallback) {
  setenv("XDG_STATE_HOME", root_.c_str(), 1);
  EXPECT_EQ(ResolveHistoryPath(), (root_ / "island/eos-repl/history").string());
  setenv("XDG_STATE_HOME", "relative", 1);
  setenv("HOME", root_.c_str(), 1);
  EXPECT_EQ(ResolveHistoryPath(), (root_ / ".local/state/island/eos-repl/history").string());
}

TEST_F(HistoryTest, CreatesAndSecuresHistoryLocation) {
  const auto path = root_ / "state/island/eos-repl/history";
  std::string warning;
  ASSERT_TRUE(PrepareHistoryPath(path.string(), &warning)) << warning;
  std::ofstream(path) << "status\n";
  ASSERT_TRUE(SecureHistoryFile(path.string(), &warning)) << warning;
  struct stat mode{};
  ASSERT_EQ(::stat(path.c_str(), &mode), 0);
  EXPECT_EQ(mode.st_mode & 0777, 0600);
  ASSERT_EQ(::stat(path.parent_path().c_str(), &mode), 0);
  EXPECT_EQ(mode.st_mode & 0777, 0700);
}

}  // namespace
}  // namespace eos::ext::repl
