#include "src/eos/runtime/actionlist/journal.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>

namespace eos::runtime::actionlist {
namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory()
      : path(std::filesystem::temp_directory_path() /
             ("eos-actionlist-journal-test-" + std::to_string(++sequence_))) {
    std::filesystem::remove_all(path);
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path); }

  static inline std::atomic<unsigned> sequence_{0};
  std::filesystem::path path;
};

TEST(ActionListJournalTest, AppendsAndReplaysVersionedRecords) {
  TemporaryDirectory directory;
  Journal journal(directory.path);
  journal.Append("action_list", {{"workflow_id", "one"}});
  journal.Append("fact", {{"key", "gate"}, {"value", true}});

  const auto replay = journal.Replay();
  ASSERT_EQ(replay.size(), 2u);
  EXPECT_EQ(replay[0].at("version"), 1);
  EXPECT_EQ(replay[0].at("kind"), "action_list");
  EXPECT_EQ(replay[0].at("payload").at("workflow_id"), "one");
  EXPECT_EQ(replay[1].at("kind"), "fact");
}

TEST(ActionListJournalTest, RemovesTruncatedTailAndKeepsCommittedPrefix) {
  TemporaryDirectory directory;
  Journal journal(directory.path);
  journal.Append("first", {1});
  const auto prefix_size = std::filesystem::file_size(journal.path());
  journal.Append("second", {2});
  std::filesystem::resize_file(journal.path(), std::filesystem::file_size(journal.path()) - 3);

  const auto replay = journal.Replay();
  ASSERT_EQ(replay.size(), 1u);
  EXPECT_EQ(replay.front().at("kind"), "first");
  EXPECT_EQ(std::filesystem::file_size(journal.path()), prefix_size);
}

TEST(ActionListJournalTest, RemovesChecksumInvalidTailAndRejectsInteriorCorruption) {
  TemporaryDirectory directory;
  Journal journal(directory.path);
  journal.Append("first", {1});
  const auto prefix_size = std::filesystem::file_size(journal.path());
  journal.Append("second", {2});
  {
    std::fstream file(journal.path(), std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(-1, std::ios::end);
    const char corrupt = 'x';
    file.write(&corrupt, 1);
  }
  ASSERT_EQ(journal.Replay().size(), 1u);
  EXPECT_EQ(std::filesystem::file_size(journal.path()), prefix_size);

  journal.Append("second", {2});
  journal.Append("third", {3});
  {
    std::fstream file(journal.path(), std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(static_cast<std::streamoff>(prefix_size + 16));
    const char corrupt = 'x';
    file.write(&corrupt, 1);
  }
  EXPECT_THROW(journal.Replay(), std::runtime_error);
}

TEST(ActionListJournalTest, EnforcesRecordAndJournalBounds) {
  TemporaryDirectory directory;
  Journal journal(directory.path, {64, 96});
  EXPECT_THROW(journal.Append("large", {{"value", std::string(128, 'x')}}), std::length_error);
}

TEST(ActionListJournalTest, AtomicallyRewritesAuthoritativeRecords) {
  TemporaryDirectory directory;
  Journal journal(directory.path);
  journal.Append("action_list", {{"workflow_id", "obsolete"}});
  journal.Rewrite({{{"kind", "action_list"},
                    {"payload", {{"workflow_id", "current"}}}},
                   {{"kind", "fact"}, {"payload", {{"key", "gate"}, {"value", true}}}}});

  const auto replay = journal.Replay();
  ASSERT_EQ(replay.size(), 2u);
  EXPECT_EQ(replay.front().at("payload").at("workflow_id"), "current");
  EXPECT_EQ(replay.back().at("payload").at("key"), "gate");
}

}  // namespace
}  // namespace eos::runtime::actionlist
