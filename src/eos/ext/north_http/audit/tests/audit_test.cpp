#include "src/eos/ext/north_http/audit/audit.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <spdlog/sinks/base_sink.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "eos/workflow.grpc.pb.h"
#include "src/eos/ext/north_http/client/client.h"
#include "src/eos/lib/log.h"

namespace eos::ext::north_http::audit {
namespace {

namespace Proto = island::eos::v1;
using namespace std::chrono_literals;

class WorkflowFixture final : public Proto::WorkflowService::Service {
 public:
  grpc::Status GetRuntimeSnapshot(grpc::ServerContext*, const Proto::GetRuntimeSnapshotRequest*,
                                  Proto::ManagerRuntimeSnapshot* response) override {
    ++snapshots;
    while (hold_snapshot.load()) std::this_thread::sleep_for(1ms);
    if (snapshot_status != grpc::StatusCode::OK)
      return grpc::Status(snapshot_status, "SNAPSHOT_SECRET");
    response->mutable_captured_at()->set_seconds(1700000000);
    response->mutable_action_list_cursor()->set_epoch(epoch);
    response->mutable_action_list_cursor()->set_sequence(snapshot_sequence.load());
    Proto::ActionList* list = response->add_action_lists();
    list->set_workflow_id("workflow-1");
    list->set_revision(1);
    list->set_status(Proto::ACTION_LIST_STATUS_RUNNING);
    list->mutable_created_at()->set_seconds(1699999990);
    list->mutable_updated_at()->set_seconds(1700000000);
    Proto::ActionItem* item = list->add_actions();
    item->mutable_action()->set_action_id("action-1");
    item->mutable_action()->mutable_skill()->set_name("wave");
    item->mutable_action()->mutable_skill()->set_version("v1");
    (*item->mutable_action()->mutable_parameters()->mutable_fields())["secret_arg"]
        .set_string_value("ARG_SECRET");
    item->set_status(Proto::ACTION_STATUS_RUNNING);
    item->set_execution_id("execution-1");
    item->set_reason("REASON_SECRET");
    (*item->mutable_result()->mutable_fields())["secret_result"].set_string_value("RESULT_SECRET");
    Proto::SystemStateFact* fact = response->add_facts();
    fact->set_key("LOW_ENTROPY_FACT_KEY");
    fact->mutable_value()->set_string_value("FACT_SECRET");
    fact->set_source("SOURCE_SECRET");
    fact->set_source_execution_id("execution-1");
    fact->set_source_sequence(9);
    fact->mutable_event_time()->set_seconds(1699999999);
    fact->mutable_received_time()->set_seconds(1700000000);
    fact->set_frame_id("FRAME_SECRET");
    fact->set_unit("UNIT_SECRET");
    return grpc::Status::OK;
  }

  grpc::Status WatchActionListEvents(grpc::ServerContext* context,
                                     const Proto::WatchActionListEventsRequest* request,
                                     grpc::ServerWriter<Proto::ActionListEvent>* writer) override {
    ++watches;
    if (request->after().epoch() != epoch)
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "epoch");
    const std::uint64_t next_sequence = event_sequence.load();
    if (request->after().sequence() < next_sequence && snapshot_sequence.load() == 0) {
      Proto::ActionListEvent event;
      event.mutable_cursor()->set_epoch(epoch);
      event.mutable_cursor()->set_sequence(next_sequence);
      event.mutable_observed_at()->set_seconds(1700000001);
      Proto::SystemStateFact* fact = event.mutable_fact();
      fact->set_key("EVENT_FACT_KEY");
      fact->mutable_value()->set_string_value("EVENT_FACT_SECRET");
      fact->set_source("EVENT_SOURCE_SECRET");
      fact->set_source_execution_id("execution-1");
      fact->set_source_sequence(10);
      fact->mutable_event_time()->set_seconds(1700000001);
      fact->mutable_received_time()->set_seconds(1700000001);
      if (advance_snapshot_after_event) snapshot_sequence.store(next_sequence);
      if (!writer->Write(event)) return grpc::Status::OK;
    }
    while (!context->IsCancelled()) std::this_thread::sleep_for(5ms);
    return grpc::Status::OK;
  }

  std::string epoch = "boot-a";
  std::atomic_uint64_t event_sequence{1};
  std::atomic_uint64_t snapshot_sequence{0};
  std::atomic_bool hold_snapshot{false};
  bool advance_snapshot_after_event = false;
  grpc::StatusCode snapshot_status = grpc::StatusCode::OK;
  std::atomic_int snapshots{0};
  std::atomic_int watches{0};
};

class TempDirectory final {
 public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    path =
        std::filesystem::temp_directory_path() / ("eos-audit-test-" + std::to_string(::getpid()) +
                                                  "-" + std::to_string(serial.fetch_add(1)));
    std::filesystem::create_directories(path);
  }
  ~TempDirectory() { std::filesystem::remove_all(path); }
  std::filesystem::path path;
};

class AuditLogCapture final {
 public:
  class Sink final : public spdlog::sinks::base_sink<std::mutex> {
   public:
    std::string str() {
      std::lock_guard<std::mutex> lock(mutex_);
      return captured_;
    }

   protected:
    void sink_it_(const spdlog::details::log_msg& message) override {
      captured_.append(message.payload.data(), message.payload.size());
      captured_ += '\n';
    }
    void flush_() override {}

   private:
    std::string captured_;
  };

  AuditLogCapture()
      : sink_(std::make_shared<Sink>()),
        logger_(eos::lib::GetLogger("north-http.audit")),
        previous_level_(logger_->level()) {
    logger_->set_level(spdlog::level::trace);
    logger_->sinks().push_back(sink_);
  }

  ~AuditLogCapture() {
    logger_->sinks().erase(std::remove(logger_->sinks().begin(), logger_->sinks().end(), sink_),
                           logger_->sinks().end());
    logger_->set_level(previous_level_);
  }

  std::string str() { return sink_->str(); }

 private:
  std::shared_ptr<Sink> sink_;
  std::shared_ptr<spdlog::logger> logger_;
  spdlog::level::level_enum previous_level_;
};

bool WaitFor(const std::function<bool()>& predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

std::filesystem::path LastSegment(const std::filesystem::path& directory) {
  std::filesystem::path result;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() == ".jsonl" &&
        entry.path().filename().string() > result.filename().string())
      result = entry.path();
  }
  return result;
}

class AuditTest : public ::testing::Test {
 protected:
  void SetUp() override {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(&workflow_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    client_ = std::make_unique<Client>("127.0.0.1:" + std::to_string(port_));
  }

  void TearDown() override {
    client_.reset();
    server_->Shutdown();
    server_->Wait();
  }

  Config MakeConfig(const std::filesystem::path& path) {
    Config config;
    config.directory = path;
    config.reconnect_delay = 20ms;
    return config;
  }

  void Populate(const std::filesystem::path& path) {
    Service service(*client_, MakeConfig(path));
    service.Start();
    ASSERT_TRUE(WaitFor([&] { return service.status().sequence == 1; }));
    service.Stop();
  }

  WorkflowFixture workflow_;
  int port_ = 0;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<Client> client_;
};

TEST_F(AuditTest, ProjectsOnlyAllowlistedFieldsAndResumesWithoutDuplicates) {
  TempDirectory directory;
  Service first(*client_, MakeConfig(directory.path));
  first.Start();
  ASSERT_TRUE(WaitFor([&] { return first.status().sequence == 1; }));
  first.Stop();
  const auto initial = first.record_page(0).records;
  ASSERT_EQ(initial.size(), 2U);
  const std::size_t newest_bytes = initial.back().dump().size();
  const RecordPage truncated = first.record_page(100, newest_bytes + 2);
  ASSERT_EQ(truncated.records.size(), 1U);
  EXPECT_TRUE(truncated.truncated);
  EXPECT_FALSE(truncated.oversized);
  EXPECT_EQ(truncated.encoded_bytes, newest_bytes + 2);
  const RecordPage oversized = first.record_page(100, newest_bytes + 1);
  EXPECT_TRUE(oversized.records.empty());
  EXPECT_TRUE(oversized.truncated);
  EXPECT_TRUE(oversized.oversized);
  struct stat directory_stat{};
  struct stat segment_stat{};
  struct stat cursor_stat{};
  ASSERT_EQ(::stat(directory.path.c_str(), &directory_stat), 0);
  ASSERT_EQ(::stat(LastSegment(directory.path).c_str(), &segment_stat), 0);
  ASSERT_EQ(::stat((directory.path / "cursor.json").c_str(), &cursor_stat), 0);
  EXPECT_EQ(directory_stat.st_mode & 0777, 0700);
  EXPECT_EQ(segment_stat.st_mode & 0777, 0600);
  EXPECT_EQ(cursor_stat.st_mode & 0777, 0600);
  const std::string encoded = nlohmann::json(initial).dump();
  for (const char* secret :
       {"ARG_SECRET", "RESULT_SECRET", "REASON_SECRET", "FACT_SECRET", "LOW_ENTROPY_FACT_KEY",
        "SOURCE_SECRET", "FRAME_SECRET", "UNIT_SECRET", "parameters", "result", "reason", "value"})
    EXPECT_EQ(encoded.find(secret), std::string::npos) << secret;

  Service resumed(*client_, MakeConfig(directory.path));
  resumed.Start();
  ASSERT_TRUE(WaitFor([&] { return resumed.status().health == Health::kHealthy; }));
  resumed.Stop();
  EXPECT_EQ(resumed.status().sequence, 1U);
  EXPECT_EQ(resumed.record_page(0).records.size(), initial.size());
  EXPECT_EQ(workflow_.snapshots.load(), 1);
}

TEST_F(AuditTest, RejectsASecondWriterWithoutChangingTheJournal) {
  TempDirectory directory;
  Service first(*client_, MakeConfig(directory.path));
  first.Start();
  ASSERT_TRUE(first.status().running);
  const auto initial = first.record_page(0).records;

  Service second(*client_, MakeConfig(directory.path));
  second.Start();
  EXPECT_EQ(second.status().health, Health::kDegraded);
  EXPECT_EQ(second.status().error, ErrorCode::kWriterConflict);
  EXPECT_FALSE(second.status().running);
  EXPECT_TRUE(second.record_page(0).records.empty());
  EXPECT_EQ(first.record_page(0).records, initial);

  struct stat lock_stat{};
  ASSERT_EQ(::stat((directory.path / "writer.lock").c_str(), &lock_stat), 0);
  EXPECT_EQ(lock_stat.st_mode & 0777, 0600);

  first.Stop();
  Service resumed(*client_, MakeConfig(directory.path));
  resumed.Start();
  ASSERT_TRUE(resumed.status().running);
  resumed.Stop();
}

TEST_F(AuditTest, RejectsAppendWhenHeldWriterLockIsReplaced) {
  TempDirectory directory;
  workflow_.hold_snapshot.store(true);
  Service service(*client_, MakeConfig(directory.path));
  service.Start();
  const bool snapshot_started = WaitFor([&] { return workflow_.snapshots.load() > 0; });
  std::error_code error;
  const auto lock_path = directory.path / "writer.lock";
  std::filesystem::remove(lock_path, error);
  EXPECT_FALSE(error);
  std::ofstream(lock_path) << "replacement";
  workflow_.hold_snapshot.store(false);

  EXPECT_TRUE(snapshot_started);
  EXPECT_TRUE(WaitFor([&] { return service.status().health == Health::kDegraded; }));
  service.Stop();
  EXPECT_EQ(service.status().error, ErrorCode::kWriterConflict);
  EXPECT_TRUE(service.record_page(0).records.empty());
  EXPECT_TRUE(LastSegment(directory.path).empty());
}

TEST_F(AuditTest, TruncatesUnterminatedMalformedTail) {
  TempDirectory directory;
  Populate(directory.path);
  {
    std::ofstream output(LastSegment(directory.path), std::ios::app | std::ios::binary);
    output << "{\"kind\":\"torn";
  }
  Service recovered(*client_, MakeConfig(directory.path));
  recovered.Start();
  ASSERT_TRUE(WaitFor([&] { return recovered.status().health == Health::kHealthy; }));
  recovered.Stop();
  EXPECT_EQ(recovered.status().sequence, 1U);
  EXPECT_EQ(recovered.record_page(0).records.size(), 2U);
}

TEST_F(AuditTest, RejectsCompleteTailCorruption) {
  TempDirectory directory;
  Populate(directory.path);
  {
    std::ofstream output(LastSegment(directory.path), std::ios::app | std::ios::binary);
    output << "{\"kind\":\"complete-corrupt\"}\n";
  }
  Service rejected(*client_, MakeConfig(directory.path));
  rejected.Start();
  EXPECT_EQ(rejected.status().health, Health::kDegraded);
  EXPECT_EQ(rejected.status().error, ErrorCode::kCorruptChain);
  EXPECT_FALSE(rejected.status().running);
}

TEST_F(AuditTest, RejectsInteriorChainCorruption) {
  TempDirectory directory;
  Populate(directory.path);
  const std::filesystem::path segment = LastSegment(directory.path);
  std::ifstream input(segment, std::ios::binary);
  std::string first;
  std::string second;
  ASSERT_TRUE(static_cast<bool>(std::getline(input, first)));
  ASSERT_TRUE(static_cast<bool>(std::getline(input, second)));
  input.close();
  nlohmann::json record = nlohmann::json::parse(second);
  record["previous_hash"] = std::string(64, '0');
  std::ofstream output(segment, std::ios::binary | std::ios::trunc);
  output << first << '\n' << record.dump() << '\n';
  output.close();
  AuditLogCapture captured;
  Service rejected(*client_, MakeConfig(directory.path));
  rejected.Start();
  EXPECT_EQ(rejected.status().health, Health::kDegraded);
  EXPECT_EQ(rejected.status().error, ErrorCode::kCorruptChain);
  const std::string log = captured.str();
  EXPECT_NE(log.find("stage=recovery_predecessor_mismatch"), std::string::npos);
  EXPECT_NE(log.find("segment 0, record 2"), std::string::npos);
}

TEST_F(AuditTest, RejectsParseableUnterminatedTampering) {
  TempDirectory directory;
  Populate(directory.path);
  {
    std::ofstream output(LastSegment(directory.path), std::ios::app | std::ios::binary);
    output << "{\"kind\":\"parseable\",\"hash\":\"bad\",\"previous_hash\":\"bad\"}";
  }
  Service rejected(*client_, MakeConfig(directory.path));
  rejected.Start();
  EXPECT_EQ(rejected.status().health, Health::kDegraded);
  EXPECT_EQ(rejected.status().error, ErrorCode::kCorruptChain);
}

TEST_F(AuditTest, ReplaysAValidButUnterminatedLastRecord) {
  TempDirectory directory;
  Populate(directory.path);
  const std::filesystem::path segment = LastSegment(directory.path);
  const auto size = std::filesystem::file_size(segment);
  ASSERT_GT(size, 1U);
  std::filesystem::resize_file(segment, size - 1);
  Service recovered(*client_, MakeConfig(directory.path));
  recovered.Start();
  ASSERT_TRUE(WaitFor([&] { return recovered.status().sequence == 1; }));
  recovered.Stop();
  EXPECT_EQ(recovered.record_page(0).records.size(), 2U);
}

TEST_F(AuditTest, RejectsInteriorBlankLine) {
  TempDirectory directory;
  Populate(directory.path);
  const std::filesystem::path segment = LastSegment(directory.path);
  std::ifstream input(segment, std::ios::binary);
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  const std::size_t newline = contents.find('\n');
  ASSERT_NE(newline, std::string::npos);
  input.close();
  std::ofstream output(segment, std::ios::binary | std::ios::trunc);
  output << contents.substr(0, newline + 1) << '\n' << contents.substr(newline + 1);
  output.close();
  Service rejected(*client_, MakeConfig(directory.path));
  rejected.Start();
  EXPECT_EQ(rejected.status().health, Health::kDegraded);
  EXPECT_EQ(rejected.status().error, ErrorCode::kCorruptChain);
}

TEST_F(AuditTest, RejectsSegmentSymlinksWithoutFollowingThem) {
  TempDirectory directory;
  TempDirectory outside;
  const std::filesystem::path target = outside.path / "outside.jsonl";
  {
    std::ofstream output(target, std::ios::binary);
    output << "outside";
  }
  std::filesystem::permissions(target, std::filesystem::perms::owner_read);
  std::filesystem::create_symlink(target, directory.path / "audit-000000000000.jsonl");

  Service rejected(*client_, MakeConfig(directory.path));
  rejected.Start();
  EXPECT_EQ(rejected.status().health, Health::kDegraded);
  EXPECT_EQ(rejected.status().error, ErrorCode::kIo);
  EXPECT_EQ(std::filesystem::status(target).permissions() & std::filesystem::perms::owner_write,
            std::filesystem::perms::none);
}

TEST_F(AuditTest, RejectsOversizedSegmentBeforeMaterializingIt) {
  TempDirectory directory;
  {
    std::ofstream output(directory.path / "audit-000000000000.jsonl", std::ios::binary);
    output << std::string(65, 'X');
  }
  Config config = MakeConfig(directory.path);
  config.rotation_size_bytes = 64;
  Service service(*client_, config);
  service.Start();

  EXPECT_EQ(service.status().health, Health::kDegraded);
  EXPECT_EQ(service.status().error, ErrorCode::kResourceExhausted);
  EXPECT_TRUE(service.record_page(0).records.empty());
}

TEST_F(AuditTest, RejectsTooManySegmentsBeforeMaterializingThem) {
  TempDirectory directory;
  std::ofstream(directory.path / "audit-000000000000.jsonl", std::ios::binary);
  std::ofstream(directory.path / "audit-000000000001.jsonl", std::ios::binary);
  Config config = MakeConfig(directory.path);
  config.rotation_files = 1;
  Service service(*client_, config);
  service.Start();

  EXPECT_EQ(service.status().health, Health::kDegraded);
  EXPECT_EQ(service.status().error, ErrorCode::kResourceExhausted);
  EXPECT_TRUE(service.record_page(0).records.empty());
}

TEST_F(AuditTest, RotatesRetainsAndRecoversTheLatestChainSuffix) {
  TempDirectory directory;
  Config config = MakeConfig(directory.path);
  config.rotation_size_bytes = 800;
  config.rotation_files = 1;
  Service first(*client_, config);
  first.Start();
  ASSERT_TRUE(WaitFor([&] { return first.status().sequence == 1; }));
  first.Stop();
  std::size_t segments = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory.path))
    if (entry.path().extension() == ".jsonl") ++segments;
  EXPECT_EQ(segments, 1U);
  EXPECT_EQ(first.record_page(0).records.size(), 1U);

  Service recovered(*client_, config);
  recovered.Start();
  ASSERT_TRUE(WaitFor([&] { return recovered.status().health == Health::kHealthy; }));
  recovered.Stop();
  EXPECT_EQ(recovered.status().sequence, 1U);
  EXPECT_EQ(recovered.record_page(0).records.size(), 1U);
}

TEST_F(AuditTest, RejectsRecordLargerThanItsSegmentWithoutAppending) {
  TempDirectory directory;
  Config config = MakeConfig(directory.path);
  config.rotation_size_bytes = 64;
  AuditLogCapture captured;
  Service service(*client_, config);
  service.Start();
  ASSERT_TRUE(WaitFor([&] { return service.status().health == Health::kDegraded; }));
  service.Stop();

  EXPECT_EQ(service.status().error, ErrorCode::kResourceExhausted);
  EXPECT_TRUE(service.record_page(0).records.empty());
  EXPECT_TRUE(LastSegment(directory.path).empty());
  const std::string log = captured.str();
  EXPECT_NE(log.find("action_list_audit_record_too_large"), std::string::npos);
  EXPECT_NE(log.find("error_code=resource_exhausted"), std::string::npos);
  EXPECT_EQ(log.find("ARG_SECRET"), std::string::npos);
}

TEST_F(AuditTest, ResnapshotsOnGapAndEpochChange) {
  TempDirectory directory;
  workflow_.event_sequence.store(3);
  workflow_.advance_snapshot_after_event = true;
  Service gap(*client_, MakeConfig(directory.path));
  gap.Start();
  ASSERT_TRUE(
      WaitFor([&] { return gap.status().sequence == 3 && workflow_.snapshots.load() >= 2; }));
  gap.Stop();

  workflow_.epoch = "boot-b";
  workflow_.snapshot_sequence.store(5);
  Service epoch(*client_, MakeConfig(directory.path));
  epoch.Start();
  ASSERT_TRUE(
      WaitFor([&] { return epoch.status().epoch == "boot-b" && epoch.status().sequence == 5; }));
  epoch.Stop();
  EXPECT_EQ(epoch.status().health, Health::kStopped);
}

TEST_F(AuditTest, LogsSnapshotRetryWithOnlyStableStatus) {
  TempDirectory directory;
  workflow_.snapshot_status = grpc::StatusCode::UNAVAILABLE;
  AuditLogCapture captured;

  Service service(*client_, MakeConfig(directory.path));
  service.Start();
  EXPECT_TRUE(WaitFor([&] {
    return captured.str().find("action_list_audit_snapshot_retry") != std::string::npos;
  }));
  service.Stop();

  const std::string log = captured.str();
  EXPECT_NE(log.find("action_list_audit_snapshot_retry"), std::string::npos);
  EXPECT_NE(log.find("grpc_status=14"), std::string::npos);
  EXPECT_EQ(log.find("SNAPSHOT_SECRET"), std::string::npos);
}

TEST_F(AuditTest, RejectsEmptySnapshotEpochBeforeAppend) {
  TempDirectory directory;
  workflow_.epoch.clear();
  AuditLogCapture captured;

  Service service(*client_, MakeConfig(directory.path));
  service.Start();
  EXPECT_TRUE(WaitFor([&] {
    return captured.str().find("action_list_audit_snapshot_retry") != std::string::npos;
  }));
  service.Stop();

  EXPECT_TRUE(service.record_page(0).records.empty());
  EXPECT_EQ(service.status().sequence, 0U);
  const std::string log = captured.str();
  EXPECT_NE(log.find("action_list_audit_snapshot_retry"), std::string::npos);
  EXPECT_NE(log.find("grpc_status=0"), std::string::npos);
  EXPECT_NE(log.find(": invalid_cursor"), std::string::npos);
}

TEST(NorthHttpAuditTest, ExposesStableStatusNames) {
  EXPECT_STREQ(ToString(Health::kHealthy), "healthy");
  EXPECT_STREQ(ToString(ErrorCode::kWriterConflict), "writer_conflict");
  EXPECT_STREQ(ToString(ErrorCode::kCorruptChain), "corrupt_chain");
  EXPECT_STREQ(ToString(ErrorCode::kResourceExhausted), "resource_exhausted");
}

}  // namespace
}  // namespace eos::ext::north_http::audit
