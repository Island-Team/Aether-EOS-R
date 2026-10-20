#include "src/eos/ext/repl/options.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace eos::ext::repl {
namespace {

Args Parse(std::vector<std::string> arguments, bool* help_requested = nullptr) {
  std::vector<char*> argv;
  argv.reserve(arguments.size());
  for (std::string& argument : arguments) argv.push_back(argument.data());
  return ParseArgs(static_cast<int>(argv.size()), argv.data(), help_requested);
}

TEST(OptionsTest, UsesDefaultsWithoutAConfigFile) {
  const Args args = Parse({"eos-repl", "--config", "/definitely/missing/eos.toml"});
  EXPECT_EQ(args.host, "127.0.0.1");
  EXPECT_EQ(args.port, 16005);
  EXPECT_EQ(args.connect_timeout_ms, 2000);
  EXPECT_EQ(args.timeout_ms, 630000);
  EXPECT_TRUE(args.history);
}

TEST(OptionsTest, AppliesConfigDefaultsUnderExplicitCommandLineOverrides) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("eos-repl-options-" +
                     std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".toml");
  {
    std::ofstream output(path);
    output << "[north_http]\nhost = \"198.51.100.2\"\nport = 17005\n";
  }

  const Args configured = Parse({"eos-repl", "--config", path.string()});
  EXPECT_EQ(configured.host, "198.51.100.2");
  EXPECT_EQ(configured.port, 17005);

  const Args overridden =
      Parse({"eos-repl", "--config", path.string(), "--host", "127.0.0.9", "--port", "18003"});
  EXPECT_EQ(overridden.host, "127.0.0.9");
  EXPECT_EQ(overridden.port, 18003);
  std::filesystem::remove(path);
}

TEST(OptionsTest, ParsesAllClientOnlyOptions) {
  bool help_requested = false;
  const Args args =
      Parse({"eos-repl", "--config", "/missing", "--contracts", "/contracts",
             "--connect-timeout-ms", "17", "--timeout-ms", "23", "--eosd-pid", "42",
             "--no-history", "--help"},
            &help_requested);
  EXPECT_EQ(args.contracts, "/contracts");
  EXPECT_EQ(args.connect_timeout_ms, 17);
  EXPECT_EQ(args.timeout_ms, 23);
  EXPECT_EQ(args.eosd_pid, 42);
  EXPECT_FALSE(args.history);
  EXPECT_TRUE(help_requested);
  EXPECT_NE(std::string(Usage()).find("--connect-timeout-ms N"), std::string::npos);
}

TEST(OptionsTest, RejectsUnknownOptionsAndMissingValues) {
  EXPECT_THROW(Parse({"eos-repl", "--unknown"}), std::runtime_error);
  for (const std::string option :
       {"--host", "--port", "--config", "--contracts", "--connect-timeout-ms", "--timeout-ms",
        "--eosd-pid"}) {
    EXPECT_THROW(Parse({"eos-repl", option}), std::runtime_error) << option;
    EXPECT_THROW(Parse({"eos-repl", option, "--no-history"}), std::runtime_error) << option;
  }
}

TEST(OptionsTest, RejectsMalformedOrOutOfRangeNumbers) {
  for (const std::string value : {"", "nope", "12junk", "0", "-1", "999999999999999999999"}) {
    EXPECT_THROW(Parse({"eos-repl", "--timeout-ms", value}), std::runtime_error) << value;
    EXPECT_THROW(Parse({"eos-repl", "--connect-timeout-ms", value}), std::runtime_error) << value;
    EXPECT_THROW(Parse({"eos-repl", "--eosd-pid", value}), std::runtime_error) << value;
  }
  for (const std::string value : {"nope", "12junk", "0", "-1", "65536"}) {
    EXPECT_THROW(Parse({"eos-repl", "--port", value}), std::runtime_error) << value;
  }
}

}  // namespace
}  // namespace eos::ext::repl
