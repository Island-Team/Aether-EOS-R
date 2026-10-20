#include "src/eos/lib/log.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace eos::lib {
namespace {

class EnvironmentGuard {
 public:
  EnvironmentGuard() {
    for (const char* name :
         {"ISLAND_LOG", "EOS_VAR", "ISLAND_MODE", "ISLAND_PREFIX", "EOS_LAUNCH_ID",
          "ISLAND_LOG_DIR"}) {
      const char* value = std::getenv(name);
      values_.emplace(name, value == nullptr ? std::nullopt : std::optional<std::string>(value));
      unsetenv(name);
    }
  }
  ~EnvironmentGuard() {
    for (const auto& [name, value] : values_) {
      if (value)
        setenv(name.c_str(), value->c_str(), 1);
      else
        unsetenv(name.c_str());
    }
  }

 private:
  std::map<std::string, std::optional<std::string>> values_;
};

std::string Read(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::string ReadFamily(const std::filesystem::path& path, std::uint32_t retained_files) {
  std::string contents;
  for (std::uint32_t index = retained_files; index > 0; --index) {
    contents += Read(path.string() + "." + std::to_string(index));
  }
  contents += Read(path);
  return contents;
}

std::vector<nlohmann::json> ReadJsonl(const std::filesystem::path& path,
                                      std::uint32_t retained_files) {
  std::istringstream input(ReadFamily(path, retained_files));
  std::vector<nlohmann::json> records;
  for (std::string line; std::getline(input, line);) {
    if (!line.empty()) records.push_back(nlohmann::json::parse(line));
  }
  return records;
}

TEST(LogTest, ResolvesDefaultDirectoryForStandaloneProcesses) {
  EnvironmentGuard environment;
  EXPECT_EQ(DefaultLogDirectory(), std::filesystem::path("var/log/eos"));

  setenv("ISLAND_PREFIX", "/opt/island", 1);
  EXPECT_EQ(DefaultLogDirectory(), std::filesystem::path("/opt/island/var/log/eos"));

  setenv("ISLAND_MODE", "system", 1);
  EXPECT_EQ(DefaultLogDirectory(), std::filesystem::path("/var/log/island/eos"));

  setenv("EOS_VAR", "/srv/island", 1);
  EXPECT_EQ(DefaultLogDirectory(), std::filesystem::path("/srv/island/log/eos"));

  setenv("ISLAND_LOG", "/logs/island", 1);
  EXPECT_EQ(DefaultLogDirectory(), std::filesystem::path("/logs/island/eos"));

  setenv("ISLAND_LOG", "relative/logs", 1);
  EXPECT_THROW(DefaultLogDirectory(), std::invalid_argument);
}

TEST(LogTest, ReusesGeneratedStandaloneLaunchIdWithinProcess) {
  EnvironmentGuard environment;
  const auto first = ProcessLogDirectory("/var/log/eos", "eosd");
  const auto second = ProcessLogDirectory("/var/log/eos", "simulator");
  EXPECT_EQ(first.parent_path(), second.parent_path());
  EXPECT_NE(first.parent_path().filename(), "current");
}

TEST(LogTest, ReusableSanitizerMatchesStructuredLoggingPolicy) {
  const auto sanitized = SanitizeLogFields(
      {{"api_token", "secret"},
       {"requestPayload", "payload"},
       {"speech_active", true},
       {"audio_events", 3},
       {"audio_event_count", 4},
       {"audio_events_raw", "private audio"},
       {"endpoint", "https://user:pass@example.test/path?q=secret"},
       {"nested", {{"status", "ready\nnow"}}}});
  EXPECT_EQ(sanitized.at("api_token"), "[REDACTED]");
  EXPECT_EQ(sanitized.at("requestPayload"), "[REDACTED]");
  EXPECT_EQ(sanitized.at("speech_active"), true);
  EXPECT_EQ(sanitized.at("audio_events"), 3);
  EXPECT_EQ(sanitized.at("audio_event_count"), 4);
  EXPECT_EQ(sanitized.at("audio_events_raw"), "[REDACTED]");
  EXPECT_EQ(sanitized.at("endpoint"), "https://example.test/path");
  EXPECT_EQ(sanitized.at("nested").at("status"), "ready now");
  EXPECT_EQ(SanitizeLogEvent("line\nbreak"), "line break");
  EXPECT_EQ(SanitizeLogEvent(std::string(256, 'x')).size(), 128U);
}

// ConfigureLogging is deliberately once-per-process, so all sink behavior is
// exercised in one test against a single directory.
TEST(LogTest, StructuredRoutedSanitizedCorrelatedAndDeduplicated) {
  const LogConfig defaults;
  EXPECT_EQ(defaults.console_level, "off");
  EXPECT_EQ(defaults.file_level, "info");
  EXPECT_EQ(defaults.jsonl_level, "trace");
  EXPECT_FALSE(defaults.overwrite);
  EXPECT_TRUE(defaults.dedup);
  EXPECT_DOUBLE_EQ(defaults.dedup_window_s, 30.0);
  EXPECT_FALSE(defaults.save_attachments);
  EXPECT_EQ(defaults.rotation_size_bytes, 64ULL * 1024ULL * 1024ULL);
  EXPECT_EQ(defaults.rotation_files, 5U);

  setenv("EOS_LAUNCH_ID", "launch-test", 1);
  EXPECT_EQ(ProcessLogDirectory("/var/log/eos", "north-http"),
            std::filesystem::path("/var/log/eos/runs/launch-test/north-http"));
  EXPECT_EQ(ProcessLogDirectory("/var/log/eos", "north-http", true),
            std::filesystem::path("/var/log/eos/runs/current/north-http"));
  EXPECT_THROW(ProcessLogDirectory("/var/log/eos", "../shared"), std::invalid_argument);
  setenv("EOS_LAUNCH_ID", "../shared", 1);
  EXPECT_THROW(ProcessLogDirectory("/var/log/eos", "north-http"), std::invalid_argument);
  setenv("EOS_LAUNCH_ID", "launch-test", 1);

  const auto preconfigured = GetLogger("hardware.preconfigured");
  const std::filesystem::path root = std::filesystem::temp_directory_path() / "eos_log_test";
  const std::filesystem::path dir = ProcessLogDirectory(root, "unit-test");
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(dir / "attachments");
  std::ofstream(dir / "runtime-body.log") << "stale body error\n";
  std::ofstream(dir / "attachments/asr-stale.wav") << "stale attachment\n";
  std::ofstream(dir / "keep.txt") << "preserve me\n";
  LogConfig config;
  config.console_level = "off";
  config.file_level = "trace";
  config.jsonl_level = "trace";
  config.dedup_window_s = 0.1;
  config.rotation_size_bytes = 8192;
  ConfigureLogging(root, "unit-test", config);
  ASSERT_NE(std::getenv("ISLAND_LOG_DIR"), nullptr);
  EXPECT_EQ(std::getenv("ISLAND_LOG_DIR"), dir.string());
  EXPECT_EQ(CurrentLogProcessContext().launch_id, "launch-test");
  EXPECT_EQ(std::filesystem::read_symlink(root / "latest"),
            std::filesystem::path("runs/launch-test"));
  EXPECT_THROW(ConfigureLogAttachments(dir / "unsafe", true), std::invalid_argument);
  ConfigureLogAttachments(dir / "attachments", true);
  ConfigureLogging(dir, "debug");  // idempotent no-op
  ConfigureLogging(root / "other-root", "other-service", config);
  EXPECT_FALSE(std::filesystem::exists(root / "other-root/latest"));
  EXPECT_EQ(std::getenv("ISLAND_LOG_DIR"), dir.string());
  EXPECT_NE(Read(dir / "runtime-body.log").find("stale body error"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(dir / "attachments/asr-stale.wav"));
  EXPECT_TRUE(std::filesystem::exists(dir / "keep.txt"));

  preconfigured->info("legacy_event");
  GetLogger("foreign.library")->info("thirdparty_event");
  LogEvent(GetLogger("microphone"), spdlog::level::info, "microphone_backend_started",
           {{"device", "mic-0"}});
  LogEventWithAttachment(GetLogger("models"), spdlog::level::info, "attachment_test",
                         {{"kind", "audio"}}, "asr", ".wav", "audio/wav",
                         std::vector<std::uint8_t>{'R', 'I', 'F', 'F'});
  LogEventWithAttachment(GetLogger("models"), spdlog::level::info, "attachment_duplicate_test",
                         {{"kind", "audio"}}, "asr", ".wav", "audio/wav",
                         std::vector<std::uint8_t>{'R', 'I', 'F', 'F'});
  const std::array<std::pair<const char*, const char*>, 21> module_routes{{
      {"eosd", "main.log"},
      {"eos-north-http", "main.log"},
      {"eos.lib.paths", "lib.log"},
      {"hardware", "runtime-hardware.log"},
      {"dispatch", "runtime-dispatch.log"},
      {"northbound", "runtime-northbound.log"},
      {"body", "runtime-body.log"},
      {"models", "runtime-models.log"},
      {"skills", "runtime-skills.log"},
      {"actionlist", "runtime-actionlist.log"},
      {"contracts", "runtime-contracts.log"},
      {"init", "runtime-init.log"},
      {"camera", "ext-camera.log"},
      {"north-http.audit", "ext-north-http-audit.log"},
      {"north-http.client", "ext-north-http-client.log"},
      {"eos-repl", "ext-repl.log"},
      {"ros2", "ext-ros2.log"},
      {"speaker", "ext-speaker.log"},
      {"microphone", "ext-alsa.log"},
      {"vendor", "ext-vendor.log"},
      {"demo", "ext-demo.log"},
  }};
  for (const auto& [logger, route] : module_routes) {
    LogEvent(GetLogger(logger), spdlog::level::info, "module_route_test", {{"route", route}});
  }
  const auto bridge = GetLogger("hardware");
  {
    CorrelationScope outer("outer-id");
    LogEvent(bridge, spdlog::level::info, "device_admitted",
             {{"device_id", "arm\n1"},
              {"api_token", "never-write-this"},
              {"request_payload", "never-write-payload"},
              {"requestPayload", "never-write-camel-payload"},
              {"nested",
               {{"arguments", {{"prompt", "never-write-prompt"}}},
                {"image_b64", "never-write-image"},
                {"audio_base64", "never-write-audio"},
                {"rawData", "never-write-data"},
                {"speech", "never-write-speech"},
                {"schema", {{"contents", "never-write-contents"}}},
                {"result", "never-write-result"},
                {"transcript", "never-write-transcript"},
                {"image_bytes", 42}}},
              {"endpoint", "https://user:pass@example.test/path?q=secret#fragment"}});
    EXPECT_NE(Read(dir / "runtime-hardware.log").find("device_admitted"), std::string::npos);
    ASSERT_FALSE(ReadJsonl(dir / "events.jsonl", config.rotation_files).empty());
    {
      CorrelationScope inner("inner-id");
      EXPECT_EQ(CurrentCorrelationId(), "inner-id");
      LogEvent(bridge, spdlog::level::debug, "nested_scope", {{"ok", true}});
    }
    EXPECT_EQ(CurrentCorrelationId(), "outer-id");
  }
  EXPECT_TRUE(CurrentCorrelationId().empty());

  LogOnce(bridge, spdlog::level::info, "startup-key", "one_shot", {{"count", 1}});
  LogOnce(bridge, spdlog::level::info, "startup-key", "one_shot", {{"count", 2}});
  LogEvent(bridge, spdlog::level::warn, "repeated_failure", {{"code", 7}});
  LogEvent(bridge, spdlog::level::warn, "repeated_failure", {{"code", 7}});
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  LogEvent(bridge, spdlog::level::warn, "repeated_failure", {{"code", 7}});
  LogEvent(bridge, spdlog::level::warn, "continuous_failure", {{"code", 8}});
  LogEvent(bridge, spdlog::level::err, "vendor_navigation_rejected",
           {{"reason", "vendor rejected command"}, {"status_code", 41745}},
           "Navigation failed");
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  LogEvent(bridge, spdlog::level::warn, "continuous_failure", {{"code", 8}});
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  LogEvent(bridge, spdlog::level::warn, "continuous_failure", {{"code", 8}});
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  LogEvent(bridge, spdlog::level::warn, "continuous_failure", {{"code", 8}});

  std::vector<std::thread> workers;
  for (int i = 0; i < 4; ++i) {
    workers.emplace_back([bridge, i] {
      CorrelationScope scope("worker-" + std::to_string(i));
      LogEvent(bridge, spdlog::level::info, "worker_event", {{"worker", i}});
    });
  }
  for (auto& worker : workers) worker.join();
  for (int index = 0; index < 12; ++index) {
    LogEvent(bridge, spdlog::level::info, "rotation_test",
             {{"index", index}, {"padding", std::string(1024, 'x')}});
  }
  spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) { logger->flush(); });

  const auto records = ReadJsonl(dir / "events.jsonl", config.rotation_files);
  ASSERT_FALSE(records.empty());
  bool saw_structured = false;
  bool saw_nested = false;
  bool saw_repeat_count = false;
  int one_shot_count = 0;
  int continuous_failure_count = 0;
  int worker_count = 0;
  std::filesystem::path attachment_path;
  std::filesystem::path duplicate_attachment_path;
  for (const auto& record : records) {
    EXPECT_TRUE(record.contains("ts"));
    EXPECT_TRUE(record.contains("level"));
    EXPECT_TRUE(record.contains("logger"));
    EXPECT_TRUE(record.contains("msg"));
    EXPECT_EQ(record.value("service", ""), "unit-test");
    EXPECT_EQ(record.value("version", ""), "1.0.0");
    EXPECT_EQ(record.value("launch_id", ""), "launch-test");
    EXPECT_EQ(record.value("run_id", ""), CurrentLogProcessContext().run_id);
    EXPECT_EQ(record.value("pid", 0ULL), CurrentLogProcessContext().pid);
    if (record.value("event", "") == "device_admitted") {
      saw_structured = true;
      EXPECT_EQ(record.at("correlation_id"), "outer-id");
      EXPECT_EQ(record.at("fields").at("api_token"), "[REDACTED]");
      EXPECT_EQ(record.at("fields").at("request_payload"), "[REDACTED]");
      EXPECT_EQ(record.at("fields").at("requestPayload"), "[REDACTED]");
      const auto& nested = record.at("fields").at("nested");
      EXPECT_EQ(nested.at("arguments"), "[REDACTED]");
      EXPECT_EQ(nested.at("image_b64"), "[REDACTED]");
      EXPECT_EQ(nested.at("audio_base64"), "[REDACTED]");
      EXPECT_EQ(nested.at("rawData"), "[REDACTED]");
      EXPECT_EQ(nested.at("speech"), "[REDACTED]");
      EXPECT_EQ(nested.at("schema"), "[REDACTED]");
      EXPECT_EQ(nested.at("result"), "[REDACTED]");
      EXPECT_EQ(nested.at("transcript"), "[REDACTED]");
      EXPECT_EQ(nested.at("image_bytes"), 42);
      EXPECT_EQ(record.at("fields").at("device_id"), "arm 1");
      EXPECT_EQ(record.at("fields").at("endpoint"), "https://example.test/path");
    }
    if (record.value("event", "") == "nested_scope") {
      saw_nested = record.value("correlation_id", "") == "inner-id";
    }
    if (record.value("event", "") == "vendor_navigation_rejected") {
      EXPECT_EQ(record.at("fields").at("reason"), "vendor rejected command");
      EXPECT_EQ(record.at("fields").at("status_code"), 41745);
      EXPECT_EQ(record.at("msg"),
                "Navigation failed: vendor rejected command [vendor_navigation_rejected] "
                "status_code=41745");
    }
    if (record.value("event", "") == "one_shot") ++one_shot_count;
    if (record.value("event", "") == "repeated_failure" && record.value("repeats", 0) == 1)
      saw_repeat_count = true;
    if (record.value("event", "") == "continuous_failure") {
      ++continuous_failure_count;
      if (continuous_failure_count == 2) EXPECT_GE(record.value("repeats", 0), 1);
    }
    if (record.value("event", "") == "worker_event") {
      ++worker_count;
      EXPECT_EQ(record.value("correlation_id", ""),
                "worker-" + std::to_string(record.at("fields").at("worker").get<int>()));
    }
    if (record.value("event", "") == "attachment_test") {
      const auto& attachment = record.at("fields").at("attachment");
      attachment_path = attachment.at("path").get<std::string>();
      EXPECT_EQ(attachment.at("media_type"), "audio/wav");
      EXPECT_EQ(attachment.at("category"), "asr");
      EXPECT_EQ(attachment.at("relative_path").get<std::string>().find("attachments/asr-"), 0U);
      EXPECT_EQ(attachment.at("size_bytes"), 4);
    }
    if (record.value("event", "") == "attachment_duplicate_test") {
      const auto& attachment = record.at("fields").at("attachment");
      duplicate_attachment_path = attachment.at("path").get<std::string>();
      EXPECT_TRUE(attachment.at("duplicate"));
    }
  }
  EXPECT_TRUE(saw_structured);
  EXPECT_TRUE(saw_nested);
  EXPECT_TRUE(saw_repeat_count);
  EXPECT_EQ(one_shot_count, 1);
  EXPECT_GE(continuous_failure_count, 2);
  EXPECT_EQ(worker_count, 4);
  ASSERT_FALSE(attachment_path.empty());
  EXPECT_EQ(Read(attachment_path), "RIFF");
  EXPECT_EQ(duplicate_attachment_path, attachment_path);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(dir / "attachments"),
                          std::filesystem::directory_iterator()),
            1);

  const std::string human = ReadFamily(dir / "runtime-hardware.log", config.rotation_files);
  EXPECT_NE(human.find("device_admitted"), std::string::npos);
  EXPECT_NE(human.find("legacy_event"), std::string::npos);
  EXPECT_NE(human.find("api_token=[REDACTED]"), std::string::npos);
  EXPECT_NE(human.find("Repeated failure [repeated_failure] code=7 repeats=1"), std::string::npos);
  EXPECT_NE(human.find("Navigation failed: vendor rejected command "
                       "[vendor_navigation_rejected] status_code=41745"),
            std::string::npos);
  EXPECT_EQ(human.find("never-write-this"), std::string::npos);
  EXPECT_NE(Read(dir / "thirdparty.log").find("thirdparty_event"), std::string::npos);
  EXPECT_NE(Read(dir / "ext-alsa.log").find("microphone_backend_started"), std::string::npos);
  const std::string model_log = Read(dir / "runtime-models.log");
  EXPECT_NE(model_log.find("attachment_test"), std::string::npos);
  EXPECT_NE(model_log.find(attachment_path.string()), std::string::npos);
  EXPECT_NE(model_log.find("attachment_duplicate_test"), std::string::npos);
  for (const auto& [logger, route] : module_routes) {
    (void)logger;
    EXPECT_NE(ReadFamily(dir / route, config.rotation_files).find("module_route_test"),
              std::string::npos)
        << route;
  }
  EXPECT_EQ(ReadFamily(dir / "events.jsonl", config.rotation_files).find("never-write-this"),
            std::string::npos);
  const std::string all_logs =
      ReadFamily(dir / "events.jsonl", config.rotation_files) + human;
  for (const char* sentinel :
       {"never-write-payload", "never-write-camel-payload", "never-write-prompt",
        "never-write-image", "never-write-audio", "never-write-data", "never-write-speech",
        "never-write-contents", "never-write-result", "never-write-transcript"}) {
    EXPECT_EQ(all_logs.find(sentinel), std::string::npos) << sentinel;
  }
  EXPECT_TRUE(std::filesystem::exists(dir / "events.jsonl.1"));
  EXPECT_TRUE(std::filesystem::exists(dir / "runtime-hardware.log.1"));
  EXPECT_FALSE(std::filesystem::exists(dir / "events.jsonl.6"));
  EXPECT_LE(std::filesystem::file_size(dir / "events.jsonl"), config.rotation_size_bytes);

  spdlog::shutdown();
  std::filesystem::remove_all(root);
}

TEST(LogTest, ValidatesAndMintsTransportRequestIds) {
  EXPECT_TRUE(IsValidRequestId("webui:request-42.v1"));
  EXPECT_FALSE(IsValidRequestId(""));
  EXPECT_FALSE(IsValidRequestId("space is unsafe"));
  EXPECT_FALSE(IsValidRequestId(std::string(97, 'x')));
  EXPECT_EQ(ResolveRequestId("client-42"), "client-42");
  const std::string generated = ResolveRequestId();
  EXPECT_TRUE(IsValidRequestId(generated));
  EXPECT_THROW(ResolveRequestId("bad/request"), std::invalid_argument);
}

}  // namespace
}  // namespace eos::lib
