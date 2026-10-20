#include "src/eos/runtime/skills/manager/journald.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace eos::runtime::skills {
namespace {

class JournaldPersistenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    directory_ = std::filesystem::temp_directory_path() /
                 ("eos-skill-journald-" + std::to_string(++sequence_));
    std::filesystem::remove_all(directory_);
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }

  std::vector<nlohmann::json> Read(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::vector<nlohmann::json> values;
    for (std::string line; std::getline(input, line);)
      if (!line.empty()) values.push_back(nlohmann::json::parse(line));
    return values;
  }

  static inline unsigned sequence_{0};
  std::filesystem::path directory_;
};

TEST_F(JournaldPersistenceTest, WritesSanitizedLifecycleSignalAndRecordJsonl) {
  SkillJournald journal(8, directory_, true, 1024 * 1024, 2);
  detail::Record record;
  record.snapshot.execution_id = "execution-1";
  record.snapshot.request_id = "request-1";
  record.snapshot.skill_name = "test.control";
  record.snapshot.skill_version = "1.0.0";
  record.snapshot.created_at = std::chrono::system_clock::now();
  record.snapshot.result = {{"api_token", "lifecycle-secret"}};
  journal.AppendLifecycle(record, 8);

  Signal signal{"control.update", {{"api_token", "signal-secret"}}, "signal-1", "execution-1"};
  ASSERT_TRUE(journal.AppendSignal(record, std::move(signal), std::chrono::system_clock::now(), 8));
  JournalRecord diagnostic;
  diagnostic.event_id = "record-1";
  diagnostic.source = JournalSource::kSkill;
  diagnostic.kind = JournalKind::kCheckpoint;
  diagnostic.name = "checkpoint";
  diagnostic.fields = {{"api_token", "record-secret"}};
  diagnostic.timestamp = std::chrono::system_clock::now();
  ASSERT_TRUE(journal.AppendRecord(record, std::move(diagnostic), 8));
  journal.Flush();

  const auto values = Read(directory_ / "skill-diagnostics.jsonl");
  ASSERT_EQ(values.size(), 3U);
  EXPECT_EQ(values[0].at("type"), "lifecycle");
  EXPECT_FALSE(values[0].contains("result"));
  EXPECT_GT(values[0].at("output_summary").at("size_bytes"), 0U);
  EXPECT_EQ(values[0].at("output_summary").at("sha256").get<std::string>().size(), 64U);
  EXPECT_EQ(values[1].at("type"), "signal");
  EXPECT_FALSE(values[1].at("signal").contains("payload"));
  EXPECT_EQ(values[1].at("signal").at("value_summary").at("sha256").get<std::string>().size(),
            64U);
  EXPECT_EQ(values[2].at("type"), "record");
  EXPECT_EQ(values[2].at("record").at("fields").at("api_token"), "[REDACTED]");
  for (const auto& value : values)
    EXPECT_EQ(value.at("frame_sha256").get<std::string>().size(), 64U);
  const auto status = journal.persistence_status();
  EXPECT_TRUE(status.enabled);
  EXPECT_TRUE(status.healthy);
  EXPECT_EQ(status.records_written, 3U);
  DiagnosticQuery query;
  query.execution_id = "execution-1";
  query.skill_name = "test.control";
  query.severity = "info";
  EXPECT_EQ(ReadSkillDiagnostics(directory_ / "skill-diagnostics.jsonl", query).size(), 3U);
  query.skill_name = "other";
  EXPECT_TRUE(ReadSkillDiagnostics(directory_ / "skill-diagnostics.jsonl", query).empty());
}

TEST_F(JournaldPersistenceTest, RepairsInvalidCrashTailBeforeAppending) {
  detail::Record record;
  record.snapshot.execution_id = "execution-1";
  record.snapshot.skill_name = "test.control";
  record.snapshot.created_at = std::chrono::system_clock::now();
  {
    SkillJournald journal(8, directory_, false, 1024 * 1024, 2);
    journal.AppendLifecycle(record, 8);
    journal.Flush();
  }
  {
    std::ofstream output(directory_ / "skill-diagnostics.jsonl", std::ios::binary | std::ios::app);
    output << "{\"truncated\":";
  }
  {
    SkillJournald journal(8, directory_, false, 1024 * 1024, 2);
    journal.AppendLifecycle(record, 8);
    journal.Flush();
    EXPECT_TRUE(journal.persistence_status().healthy);
  }
  const auto values = ReadSkillDiagnostics(directory_ / "skill-diagnostics.jsonl");
  ASSERT_EQ(values.size(), 2U);
  EXPECT_EQ(values[0].at("execution_id"), "execution-1");
  EXPECT_EQ(values[1].at("execution_id"), "execution-1");
}

TEST_F(JournaldPersistenceTest, RotatesAndOverwriteRemovesPreviousFamily) {
  detail::Record record;
  record.snapshot.execution_id = "execution-1";
  record.snapshot.created_at = std::chrono::system_clock::now();
  {
    SkillJournald journal(8, directory_, false, 200, 2);
    for (int i = 0; i < 4; ++i) journal.AppendLifecycle(record, 8);
    journal.Flush();
  }
  EXPECT_TRUE(std::filesystem::exists(directory_ / "skill-diagnostics.jsonl.1"));

  SkillJournald replacement(8, directory_, true, 1024 * 1024, 2);
  replacement.AppendLifecycle(record, 8);
  replacement.Flush();
  EXPECT_FALSE(std::filesystem::exists(directory_ / "skill-diagnostics.jsonl.1"));
  EXPECT_EQ(Read(directory_ / "skill-diagnostics.jsonl").size(), 1U);
}

TEST_F(JournaldPersistenceTest, PersistenceFailureDoesNotChangeMemoryBehavior) {
  std::ofstream blocker(directory_);
  blocker << "not a directory";
  blocker.close();
  SkillJournald journal(8, directory_, false, 1024, 1);
  detail::Record record;
  record.snapshot.execution_id = "execution-1";
  record.snapshot.created_at = std::chrono::system_clock::now();
  EXPECT_NO_THROW(journal.AppendLifecycle(record, 8));
  journal.Flush();
  EXPECT_EQ(record.events.size(), 1U);
  EXPECT_EQ(record.lifecycle.size(), 1U);
  EXPECT_FALSE(journal.persistence_status().healthy);
  EXPECT_EQ(journal.persistence_status().records_dropped, 1U);
}

}  // namespace
}  // namespace eos::runtime::skills
