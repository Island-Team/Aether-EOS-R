#include "src/eos/ext/north_http/config/config.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <atomic>
#include <unistd.h>

namespace eos::ext::north_http {
namespace {

std::filesystem::path WriteConfig(const std::string& text) {
  static std::atomic_size_t sequence{0};
  const auto path = std::filesystem::temp_directory_path() /
                    ("eos-north-http-config-" + std::to_string(::getpid()) + "-" +
                     std::to_string(++sequence) + ".toml");
  std::ofstream(path) << text;
  return path;
}

TEST(NorthHttpConfigTest, LoadsDefaultsAndExplicitValues) {
  const auto path = WriteConfig("[north_http]\n");
  const Config config = LoadConfig(path.string());
  EXPECT_EQ(config.host, "127.0.0.1");
  EXPECT_EQ(config.port, 16005);
  EXPECT_EQ(config.upstream, "127.0.0.1:16003");
  EXPECT_FALSE(config.allow_remote);
  EXPECT_EQ(config.max_request_bytes, 64U * 1024U * 1024U);
  EXPECT_EQ(config.max_connections, 64u);
  EXPECT_EQ(config.max_inflight_requests, 32u);
  EXPECT_EQ(config.max_sse_streams, 16u);
  EXPECT_EQ(config.max_monitor_streams, 4u);
  std::filesystem::remove(path);

  const auto explicit_path = WriteConfig(
      "[north_http]\n"
      "max_connections=256\n"
      "max_inflight_requests=200\n"
      "max_sse_streams=40\n"
      "max_monitor_streams=16\n");
  const Config explicit_config = LoadConfig(explicit_path.string());
  EXPECT_EQ(explicit_config.max_connections, 256u);
  EXPECT_EQ(explicit_config.max_inflight_requests, 200u);
  EXPECT_EQ(explicit_config.max_sse_streams, 40u);
  EXPECT_EQ(explicit_config.max_monitor_streams, 16u);
  std::filesystem::remove(explicit_path);
}

TEST(NorthHttpConfigTest, StrictlyValidatesConnectionCapacities) {
  for (const char* key : {"max_connections", "max_inflight_requests", "max_sse_streams",
                          "max_monitor_streams"}) {
    for (const char* value : {"0", "-1", "'1'", "257"}) {
      const auto path = WriteConfig(std::string("[north_http]\n") + key + "=" + value + "\n");
      EXPECT_THROW(LoadConfig(path.string()), std::runtime_error) << key << "=" << value;
      std::filesystem::remove(path);
    }
  }
}

TEST(NorthHttpConfigTest, ValidatesCombinedClassCapacityWithoutOverflow) {
  const auto exact = WriteConfig(
      "[north_http]\n"
      "max_connections=256\n"
      "max_inflight_requests=254\n"
      "max_sse_streams=1\n"
      "max_monitor_streams=1\n");
  EXPECT_NO_THROW(LoadConfig(exact.string()));
  std::filesystem::remove(exact);

  const auto excessive = WriteConfig(
      "[north_http]\n"
      "max_connections=64\n"
      "max_inflight_requests=44\n"
      "max_sse_streams=16\n"
      "max_monitor_streams=5\n");
  EXPECT_THROW(LoadConfig(excessive.string()), std::runtime_error);
  std::filesystem::remove(excessive);

  const auto unary_exceeds_total = WriteConfig(
      "[north_http]\n"
      "max_connections=16\n"
      "max_inflight_requests=17\n"
      "max_sse_streams=1\n"
      "max_monitor_streams=1\n");
  EXPECT_THROW(LoadConfig(unary_exceeds_total.string()), std::runtime_error);
  std::filesystem::remove(unary_exceeds_total);

  const auto sse_exceeds_remaining = WriteConfig(
      "[north_http]\n"
      "max_connections=10\n"
      "max_inflight_requests=9\n"
      "max_sse_streams=2\n"
      "max_monitor_streams=1\n");
  EXPECT_THROW(LoadConfig(sse_exceeds_remaining.string()), std::runtime_error);
  std::filesystem::remove(sse_exceeds_remaining);
}

TEST(NorthHttpConfigTest, RejectsRemoteBindWithoutOptInAndUnknownKeys) {
  const auto remote = WriteConfig("[north_http]\nhost='0.0.0.0'\n");
  EXPECT_THROW(LoadConfig(remote.string()), std::runtime_error);
  std::filesystem::remove(remote);
  const auto unknown = WriteConfig("[north_http]\nlegacy=true\n");
  EXPECT_THROW(LoadConfig(unknown.string()), std::runtime_error);
  std::filesystem::remove(unknown);
  const auto wrong_type = WriteConfig("[north_http]\nport='16005'\n");
  EXPECT_THROW(LoadConfig(wrong_type.string()), std::runtime_error);
  std::filesystem::remove(wrong_type);
  const auto excessive_deadline = WriteConfig("[north_http]\nrequest_timeout_ms=630001\n");
  EXPECT_THROW(LoadConfig(excessive_deadline.string()), std::runtime_error);
  std::filesystem::remove(excessive_deadline);
}

TEST(NorthHttpConfigTest, RecognizesOnlyLoopbackHosts) {
  EXPECT_TRUE(IsLoopbackHost("127.0.0.1"));
  EXPECT_TRUE(IsLoopbackHost("::1"));
  EXPECT_TRUE(IsLoopbackHost("localhost"));
  EXPECT_FALSE(IsLoopbackHost("0.0.0.0"));
}

TEST(NorthHttpConfigTest, AcceptsAbsoluteUnixGrpcUpstream) {
  const auto path = WriteConfig("[north_http]\nupstream='unix:/run/island/eos.sock'\n");
  EXPECT_EQ(LoadConfig(path.string()).upstream, "unix:/run/island/eos.sock");
  std::filesystem::remove(path);

  const auto relative = WriteConfig("[north_http]\nupstream='unix:eos.sock'\n");
  EXPECT_THROW(LoadConfig(relative.string()), std::runtime_error);
  std::filesystem::remove(relative);

  EXPECT_NO_THROW(ValidateUpstream("unix:/run/island/eos.sock"));
  EXPECT_THROW(ValidateUpstream("unix:eos.sock"), std::runtime_error);
}

TEST(NorthHttpConfigTest, LoadsAndStrictlyValidatesActionListAudit) {
  const auto path = WriteConfig(
      "[north_http]\n"
      "[action_list_audit]\n"
      "enabled=true\n"
      "rotation_size_bytes=2048\n"
      "rotation_files=3\n"
      "reconnect_delay_ms=25\n");
  const Config config = LoadConfig(path.string());
  EXPECT_TRUE(config.action_list_audit.enabled);
  EXPECT_EQ(config.action_list_audit.rotation_size_bytes, 2048u);
  EXPECT_EQ(config.action_list_audit.rotation_files, 3u);
  EXPECT_EQ(config.action_list_audit.reconnect_delay_ms, 25);
  std::filesystem::remove(path);

  const auto unknown = WriteConfig("[north_http]\n[action_list_audit]\nraw_payloads=true\n");
  EXPECT_THROW(LoadConfig(unknown.string()), std::runtime_error);
  std::filesystem::remove(unknown);
  const auto zero = WriteConfig("[north_http]\n[action_list_audit]\nrotation_files=0\n");
  EXPECT_THROW(LoadConfig(zero.string()), std::runtime_error);
  std::filesystem::remove(zero);
  const auto oversized =
      WriteConfig("[north_http]\n[action_list_audit]\nrotation_size_bytes=1073741825\n");
  EXPECT_THROW(LoadConfig(oversized.string()), std::runtime_error);
  std::filesystem::remove(oversized);
  const auto excessive_retention = WriteConfig(
      "[north_http]\n[action_list_audit]\nrotation_size_bytes=134217728\nrotation_files=3\n");
  EXPECT_THROW(LoadConfig(excessive_retention.string()), std::runtime_error);
  std::filesystem::remove(excessive_retention);
  const auto too_many_files = WriteConfig("[north_http]\n[action_list_audit]\nrotation_files=65\n");
  EXPECT_THROW(LoadConfig(too_many_files.string()), std::runtime_error);
  std::filesystem::remove(too_many_files);
  const auto slow_reconnect =
      WriteConfig("[north_http]\n[action_list_audit]\nreconnect_delay_ms=60001\n");
  EXPECT_THROW(LoadConfig(slow_reconnect.string()), std::runtime_error);
  std::filesystem::remove(slow_reconnect);
}

}  // namespace
}  // namespace eos::ext::north_http
