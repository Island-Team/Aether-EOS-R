#include "src/eos/lib/paths.h"

#include <cstdlib>
#include <filesystem>

#include "gtest/gtest.h"

namespace eos::lib {
namespace {

class PathsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    unsetenv("EOS_ETC");
    unsetenv("EOS_VAR");
    unsetenv("EOS_CONTRACTS");
    unsetenv("ISLAND_PREFIX");
    unsetenv("ISLAND_MODE");
    unsetenv("ISLAND_ETC");
    unsetenv("ISLAND_SHARE");
    unsetenv("ISLAND_STATE");
    unsetenv("ISLAND_CACHE");
    unsetenv("ISLAND_LOG");
    unsetenv("ISLAND_RUN");
    unsetenv("ISLAND_TMP");
    dir_ = std::filesystem::temp_directory_path() / "eos_paths_test";
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
};

TEST_F(PathsTest, EtcRootPrefersCliArg) {
  setenv("EOS_ETC", "/nonexistent-env", 1);
  EXPECT_EQ(ResolveEtcRoot(dir_.string()), dir_);
}

TEST_F(PathsTest, EtcRootFallsBackToEnv) {
  setenv("EOS_ETC", dir_.c_str(), 1);
  EXPECT_EQ(ResolveEtcRoot(""), dir_);
}

TEST_F(PathsTest, EtcRootFallsBackToIslandNamespace) {
  std::filesystem::create_directories(dir_ / "etc" / "eos");
  setenv("ISLAND_ETC", (dir_ / "etc").c_str(), 1);
  EXPECT_EQ(ResolveEtcRoot(""), dir_ / "etc" / "eos");
}

TEST_F(PathsTest, EtcRootUnresolvableNamesCandidates) {
  try {
    ResolveEtcRoot("/nonexistent-cli");
    FAIL() << "expected ConfigRootError";
  } catch (const ConfigRootError& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("/nonexistent-cli"), std::string::npos);
  }
}

TEST_F(PathsTest, ContractsRootPrefersCliArg) {
  setenv("EOS_CONTRACTS", "/nonexistent-env", 1);
  EXPECT_EQ(ResolveContractsRoot(dir_.string()), dir_);
}

TEST_F(PathsTest, ContractsRootFallsBackToEnv) {
  setenv("EOS_CONTRACTS", dir_.c_str(), 1);
  EXPECT_EQ(ResolveContractsRoot(""), dir_);
}

TEST_F(PathsTest, ContractsRootFallsBackToIslandShare) {
  const std::filesystem::path share = dir_ / "share";
  std::filesystem::create_directories(share);
  setenv("ISLAND_SHARE", share.c_str(), 1);
  EXPECT_EQ(ResolveContractsRoot(""), share);
}

TEST_F(PathsTest, ContractsRootUnresolvableNamesCandidates) {
  setenv("EOS_CONTRACTS", "/nonexistent-env", 1);
  try {
    ResolveContractsRoot("/nonexistent-cli");
    FAIL() << "expected ConfigRootError";
  } catch (const ConfigRootError& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("/nonexistent-cli"), std::string::npos);
    EXPECT_EQ(what.find("/nonexistent-env"), std::string::npos);
  }
}

TEST_F(PathsTest, RelativeCallerRootIsRejected) {
  EXPECT_THROW(ResolveEtcRoot("etc/eos"), ConfigRootError);
  EXPECT_THROW(ResolveContractsRoot("share/contracts"), ConfigRootError);
}

TEST_F(PathsTest, LayoutUsesIslandRoleRootsAndCreatesComponentDirectories) {
  const std::filesystem::path etc = dir_ / "etc" / "eos";
  const std::filesystem::path contracts = dir_ / "share" / "contracts";
  std::filesystem::create_directories(etc);
  std::filesystem::create_directories(contracts);
  setenv("ISLAND_PREFIX", dir_.c_str(), 1);

  const LayoutPaths paths = ResolveLayoutPaths(etc.string(), contracts.string());
  EXPECT_EQ(paths.state, dir_ / "var" / "lib" / "eos");
  EXPECT_EQ(paths.cache, dir_ / "var" / "cache" / "eos");
  EXPECT_EQ(paths.log, dir_ / "var" / "log" / "eos");
  EXPECT_EQ(paths.run, dir_ / "var" / "run");
  EXPECT_EQ(paths.tmp, dir_ / "var" / "tmp");
  EXPECT_TRUE(std::filesystem::is_directory(paths.log));
}

TEST_F(PathsTest, LayoutRetainsLegacyEosVarCompatibility) {
  const std::filesystem::path etc = dir_ / "etc" / "eos";
  const std::filesystem::path contracts = dir_ / "share" / "contracts";
  const std::filesystem::path legacy = dir_ / "legacy-var";
  std::filesystem::create_directories(etc);
  std::filesystem::create_directories(contracts);
  setenv("EOS_VAR", legacy.c_str(), 1);

  const LayoutPaths paths = ResolveLayoutPaths(etc.string(), contracts.string());
  EXPECT_EQ(paths.state, legacy / "lib" / "eos");
  EXPECT_EQ(paths.log, legacy / "log" / "eos");
  EXPECT_EQ(paths.run, legacy / "run");
}

}  // namespace
}  // namespace eos::lib
