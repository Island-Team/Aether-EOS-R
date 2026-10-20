#include "src/eos/ext/north_http/audit/audit.h"

#include <cerrno>
#include <fcntl.h>
#include <google/protobuf/util/time_util.h>
#include <grpcpp/grpcpp.h>
#include <openssl/sha.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <regex>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

#include "eos/workflow.grpc.pb.h"
#include "src/eos/ext/north_http/client/client.h"
#include "src/eos/lib/log.h"

namespace eos::ext::north_http::audit {
namespace {

using json = nlohmann::json;
namespace Proto = island::eos::v1;

constexpr char kZeroHash[] = "0000000000000000000000000000000000000000000000000000000000000000";

std::shared_ptr<spdlog::logger>& Logger() {
  static auto logger = eos::lib::GetLogger("north-http.audit");
  return logger;
}

std::string Sha256(std::string_view input) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned char byte : digest) output << std::setw(2) << static_cast<unsigned int>(byte);
  return output.str();
}

std::string Timestamp(const google::protobuf::Timestamp& value) {
  return google::protobuf::util::TimeUtil::ToString(value);
}

std::string SegmentName(std::uint64_t index) {
  std::ostringstream name;
  name << "audit-" << std::setw(12) << std::setfill('0') << index << ".jsonl";
  return name.str();
}

bool IsHash(const std::string& value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool SyncFile(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  return ::close(fd) == 0 && ok;
}

bool SyncDirectory(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  return ::close(fd) == 0 && ok;
}

bool WriteSecure(const std::filesystem::path& path, std::string_view data, int flags) {
  const int fd =
      ::open(path.c_str(), flags | O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
  if (fd < 0) return false;
  bool ok = ::fchmod(fd, S_IRUSR | S_IWUSR) == 0;
  std::size_t offset = 0;
  while (ok && offset < data.size()) {
    const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
    if (written <= 0) {
      ok = false;
    } else {
      offset += static_cast<std::size_t>(written);
    }
  }
  if (ok) ok = ::fsync(fd) == 0;
  return ::close(fd) == 0 && ok;
}

json CursorFields(const Proto::ActionListCursor& cursor) {
  return {{"epoch", cursor.epoch()}, {"sequence", cursor.sequence()}};
}

json ActionListRecord(const Proto::ActionList& list, const Proto::ActionListCursor& cursor,
                      const google::protobuf::Timestamp& observed_at) {
  json targets = json::array();
  for (const Proto::ActionItem& item : list.actions()) {
    json target{{"action_id", item.action().action_id()},
                {"status", Proto::ActionStatus_Name(item.status())}};
    if (!item.execution_id().empty()) target["execution_id"] = item.execution_id();
    if (!item.command_id().empty()) target["command_id"] = item.command_id();
    if (item.action().has_skill()) {
      target["target_kind"] = "skill";
      target["target_name"] = item.action().skill().name();
      target["target_version"] = item.action().skill().version();
    } else if (item.action().has_primitive()) {
      target["target_kind"] = "primitive";
      target["target_name"] = item.action().primitive().name();
      target["target_version"] = item.action().primitive().version();
      target["device_id"] = item.action().primitive().device_id();
    }
    targets.push_back(std::move(target));
  }
  json record{{"kind", "action_list"},
              {"cursor", CursorFields(cursor)},
              {"observed_at", Timestamp(observed_at)},
              {"workflow_id", list.workflow_id()},
              {"revision", list.revision()},
              {"status", Proto::ActionListStatus_Name(list.status())},
              {"created_at", Timestamp(list.created_at())},
              {"updated_at", Timestamp(list.updated_at())},
              {"targets", std::move(targets)}};
  if (!list.superseded_by_workflow_id().empty())
    record["superseded_by_workflow_id"] = list.superseded_by_workflow_id();
  return record;
}

json FactRecord(const Proto::SystemStateFact& fact, const Proto::ActionListCursor& cursor,
                const google::protobuf::Timestamp& observed_at) {
  json record{{"kind", "fact"},
              {"cursor", CursorFields(cursor)},
              {"observed_at", Timestamp(observed_at)},
              {"event_time", Timestamp(fact.event_time())},
              {"received_time", Timestamp(fact.received_time())}};
  if (fact.source_sequence() != 0) record["source_sequence"] = fact.source_sequence();
  if (!fact.source_execution_id().empty())
    record["source_execution_id"] = fact.source_execution_id();
  return record;
}

}  // namespace

class Service::Impl {
 public:
  Impl(Client& client, Config config) : client_(client), config_(std::move(config)) {}

  ~Impl() {
    Stop();
    if (lock_fd_ >= 0) ::close(lock_fd_);
  }

  void Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;
    stop_.store(false);
    if (!ValidateConfig() || !AcquireLock() || !Recover()) {
      ReleaseLock();
      return;
    }
    running_ = true;
    health_ = Health::kStopped;
    error_ = ErrorCode::kNone;
    worker_ = std::thread([this] { Run(); });
  }

  void Stop() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_.store(true);
      if (active_context_ != nullptr) active_context_->TryCancel();
    }
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    if (health_ != Health::kDegraded) health_ = Health::kStopped;
    ReleaseLock();
  }

  Status GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {health_, error_, running_, epoch_, sequence_, record_count_};
  }

  RecordPage Page(std::size_t limit, std::size_t maximum_encoded_bytes) const {
    std::lock_guard<std::mutex> lock(mutex_);
    RecordPage page;
    std::size_t selected = 0;
    std::size_t first = records_.size();
    while (first > 0 && (limit == 0 || selected < limit)) {
      const std::size_t record_bytes = records_[first - 1].dump().size();
      const std::size_t separator_bytes = selected == 0 ? 0 : 1;
      if (page.encoded_bytes > maximum_encoded_bytes ||
          separator_bytes > maximum_encoded_bytes - page.encoded_bytes ||
          record_bytes > maximum_encoded_bytes - page.encoded_bytes - separator_bytes) {
        page.oversized = selected == 0;
        break;
      }
      page.encoded_bytes += separator_bytes + record_bytes;
      --first;
      ++selected;
    }
    page.truncated = first != 0;
    page.records.assign(records_.begin() + static_cast<std::ptrdiff_t>(first), records_.end());
    return page;
  }

 private:
  bool ValidateConfig() {
    if (config_.directory.empty() || config_.rotation_size_bytes == 0 ||
        config_.rotation_files == 0 || config_.reconnect_delay.count() <= 0) {
      Degrade(ErrorCode::kInvalidConfig, "action_list_audit_invalid_config");
      return false;
    }
    if (config_.rotation_files > kMaximumRetainedBytes / config_.rotation_size_bytes) {
      Degrade(ErrorCode::kInvalidConfig, "action_list_audit_invalid_config");
      return false;
    }
    return true;
  }

  bool AcquireLock() {
    std::error_code error;
    std::filesystem::create_directories(config_.directory, error);
    if (error || ::chmod(config_.directory.c_str(), S_IRWXU) != 0) {
      Degrade(ErrorCode::kIo, "action_list_audit_lock_failed");
      return false;
    }
    const std::filesystem::path path = config_.directory / "writer.lock";
    lock_fd_ =
        ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (lock_fd_ < 0 || ::fchmod(lock_fd_, S_IRUSR | S_IWUSR) != 0) {
      ReleaseLock();
      Degrade(ErrorCode::kIo, "action_list_audit_lock_failed");
      return false;
    }
    if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
      const int lock_error = errno;
      ReleaseLock();
      if (lock_error == EWOULDBLOCK || lock_error == EAGAIN)
        Degrade(ErrorCode::kWriterConflict, "action_list_audit_writer_conflict");
      else
        Degrade(ErrorCode::kIo, "action_list_audit_lock_failed");
      return false;
    }
    struct stat locked{};
    if (::fstat(lock_fd_, &locked) != 0) {
      ReleaseLock();
      Degrade(ErrorCode::kIo, "action_list_audit_lock_failed", "lock_identity");
      return false;
    }
    lock_device_ = locked.st_dev;
    lock_inode_ = locked.st_ino;
    return true;
  }

  bool LockIdentityMatches() const {
    if (lock_fd_ < 0) return false;
    struct stat held{};
    struct stat path{};
    const auto lock_path = config_.directory / "writer.lock";
    return ::fstat(lock_fd_, &held) == 0 && ::lstat(lock_path.c_str(), &path) == 0 &&
           S_ISREG(path.st_mode) && held.st_dev == lock_device_ && held.st_ino == lock_inode_ &&
           path.st_dev == lock_device_ && path.st_ino == lock_inode_;
  }

  void ReleaseLock() noexcept {
    if (lock_fd_ < 0) return;
    (void)::flock(lock_fd_, LOCK_UN);
    (void)::close(lock_fd_);
    lock_fd_ = -1;
  }

  void Degrade(ErrorCode error, const char* event, const char* stage = "unspecified",
               std::uint64_t segment = 0, std::uint64_t record = 0) {
    health_ = Health::kDegraded;
    error_ = error;
    running_ = false;
    json fields{{"error_code", ToString(error)}};
    if (std::string_view(stage) != "unspecified") fields["stage"] = stage;
    std::string summary;
    if (std::string_view(stage) == "recovery_empty_record")
      summary = "ActionList audit stopped: journal contains an empty record";
    else if (std::string_view(stage) == "recovery_malformed_record")
      summary = "ActionList audit stopped: journal contains malformed JSON";
    else if (std::string_view(stage) == "recovery_invalid_hash_field")
      summary = "ActionList audit stopped: journal has invalid hash metadata";
    else if (std::string_view(stage) == "recovery_predecessor_mismatch")
      summary = "ActionList audit stopped: record does not link to its predecessor";
    else if (std::string_view(stage) == "recovery_record_hash_mismatch")
      summary = "ActionList audit stopped: record content fails its hash check";
    else if (std::string_view(stage) == "append_lock_identity")
      summary = "ActionList audit stopped: writer lock was replaced";
    else if (std::string_view(stage) == "lock_identity")
      summary = "ActionList audit stopped: writer lock could not be verified";
    if (record != 0) {
      fields["segment"] = segment;
      fields["record"] = record;
      if (!summary.empty())
        summary += " at segment " + std::to_string(segment) + ", record " +
                   std::to_string(record);
    }
    eos::lib::LogEvent(Logger(), spdlog::level::err, event, fields, summary);
  }

  std::vector<std::pair<std::uint64_t, std::filesystem::path>> Segments(bool* ok) const {
    std::vector<std::pair<std::uint64_t, std::filesystem::path>> result;
    const std::regex pattern(R"(^audit-([0-9]{12})\.jsonl$)");
    std::error_code error;
    for (std::filesystem::directory_iterator it(config_.directory, error), end; !error && it != end;
         it.increment(error)) {
      std::smatch match;
      const std::string name = it->path().filename().string();
      if (!std::regex_match(name, match, pattern)) continue;
      const std::filesystem::file_status status = it->symlink_status(error);
      if (error || !std::filesystem::is_regular_file(status)) {
        *ok = false;
        return {};
      }
      result.emplace_back(std::stoull(match[1].str()), it->path());
    }
    *ok = !error;
    if (error) return {};
    std::sort(result.begin(), result.end());
    return result;
  }

  bool Recover() {
    std::error_code error;
    std::filesystem::create_directories(config_.directory, error);
    if (error) {
      Degrade(ErrorCode::kIo, "action_list_audit_recovery_failed");
      return false;
    }
    if (::chmod(config_.directory.c_str(), S_IRWXU) != 0) {
      Degrade(ErrorCode::kIo, "action_list_audit_recovery_failed");
      return false;
    }
    records_.clear();
    record_count_ = 0;
    tail_hash_ = kZeroHash;
    epoch_.clear();
    sequence_ = 0;
    bool listed = false;
    auto segments = Segments(&listed);
    if (!listed) {
      Degrade(ErrorCode::kIo, "action_list_audit_recovery_failed");
      return false;
    }
    if (segments.size() > config_.rotation_files) {
      Degrade(ErrorCode::kResourceExhausted, "action_list_audit_recovery_limit_exceeded");
      return false;
    }
    const std::uint64_t retained_limit =
        config_.rotation_size_bytes * static_cast<std::uint64_t>(config_.rotation_files);
    std::uint64_t retained_bytes = 0;
    for (const auto& [index, path] : segments) {
      (void)index;
      const std::uint64_t segment_bytes = std::filesystem::file_size(path, error);
      if (error) {
        Degrade(ErrorCode::kIo, "action_list_audit_recovery_failed");
        return false;
      }
      if (segment_bytes > config_.rotation_size_bytes ||
          retained_bytes > retained_limit - segment_bytes ||
          retained_bytes + segment_bytes > kMaximumRetainedBytes) {
        Degrade(ErrorCode::kResourceExhausted, "action_list_audit_recovery_limit_exceeded");
        return false;
      }
      retained_bytes += segment_bytes;
    }
    bool first_record = true;
    for (std::size_t segment_pos = 0; segment_pos < segments.size(); ++segment_pos) {
      segment_index_ = segments[segment_pos].first;
      if (::chmod(segments[segment_pos].second.c_str(), S_IRUSR | S_IWUSR) != 0) {
        Degrade(ErrorCode::kIo, "action_list_audit_recovery_failed");
        return false;
      }
      std::ifstream input(segments[segment_pos].second, std::ios::binary);
      if (!input) {
        Degrade(ErrorCode::kIo, "action_list_audit_recovery_failed");
        return false;
      }
      const std::string contents((std::istreambuf_iterator<char>(input)),
                                 std::istreambuf_iterator<char>());
      std::size_t start = 0;
      while (start < contents.size()) {
        const std::size_t newline = contents.find('\n', start);
        const bool partial_tail = newline == std::string::npos;
        const std::size_t end = partial_tail ? contents.size() : newline;
        const std::string line = contents.substr(start, end - start);
        if (line.empty()) {
          Degrade(ErrorCode::kCorruptChain, "action_list_audit_chain_corrupt", "recovery_empty_record",
                  segment_index_, record_count_ + 1);
          return false;
        }
        json record;
        bool valid = false;
        bool parsed = false;
        const char* failure_stage = "recovery_malformed_record";
        try {
          record = json::parse(line);
          parsed = true;
          const std::string stored_hash = record.at("hash").get<std::string>();
          const std::string previous = record.at("previous_hash").get<std::string>();
          json unhashed = record;
          unhashed.erase("hash");
          if (!IsHash(stored_hash) || !IsHash(previous)) {
            failure_stage = "recovery_invalid_hash_field";
          } else if (!first_record && previous != tail_hash_) {
            failure_stage = "recovery_predecessor_mismatch";
          } else if (stored_hash != Sha256(unhashed.dump())) {
            failure_stage = "recovery_record_hash_mismatch";
          } else {
            valid = true;
          }
        } catch (...) {
          valid = false;
        }
        const bool is_last_segment = segment_pos + 1 == segments.size();
        if (partial_tail && is_last_segment && !parsed) {
          std::filesystem::resize_file(segments[segment_pos].second, start, error);
          if (error || !SyncFile(segments[segment_pos].second)) {
            Degrade(ErrorCode::kIo, "action_list_audit_tail_recovery_failed");
            return false;
          }
          eos::lib::LogEvent(Logger(), spdlog::level::warn,
                             "action_list_audit_partial_tail_recovered");
          break;
        }
        if (partial_tail && is_last_segment && valid) {
          if (!WriteSecure(segments[segment_pos].second, "\n", O_APPEND)) {
            Degrade(ErrorCode::kIo, "action_list_audit_tail_recovery_failed");
            return false;
          }
        }
        if (!valid) {
          Degrade(ErrorCode::kCorruptChain, "action_list_audit_chain_corrupt", failure_stage,
                  segment_index_, record_count_ + 1);
          return false;
        }
        first_record = false;
        tail_hash_ = record.at("hash").get<std::string>();
        const std::string kind = record.value("kind", "");
        if (record.contains("cursor") &&
            (kind == "action_list" || kind == "fact" || kind == "snapshot_commit")) {
          epoch_ = record["cursor"].value("epoch", "");
          sequence_ = record["cursor"].value("sequence", 0ULL);
        }
        records_.push_back(std::move(record));
        ++record_count_;
        start = partial_tail ? contents.size() : newline + 1;
      }
    }
    if (segments.empty()) segment_index_ = 0;
    eos::lib::LogEvent(Logger(), spdlog::level::info, "action_list_audit_recovered",
                       {{"records", record_count_}, {"sequence", sequence_}});
    if (!WriteState()) {
      Degrade(ErrorCode::kIo, "action_list_audit_state_write_failed");
      return false;
    }
    return true;
  }

  bool WriteState() {
    const std::filesystem::path temporary = config_.directory / "cursor.json.tmp";
    const std::filesystem::path destination = config_.directory / "cursor.json";
    const json state{{"epoch", epoch_},
                     {"sequence", sequence_},
                     {"tail_hash", tail_hash_},
                     {"segment", segment_index_},
                     {"records", record_count_}};
    if (!WriteSecure(temporary, state.dump() + "\n", O_TRUNC)) return false;
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) return false;
    return SyncFile(destination) && SyncDirectory(config_.directory);
  }

  enum class AppendResult { kOk, kIo, kResourceExhausted, kLockLost };

  AppendResult Append(json record) {
    if (!LockIdentityMatches()) return AppendResult::kLockLost;
    std::filesystem::path path = config_.directory / SegmentName(segment_index_);
    std::error_code error;
    const std::uint64_t current =
        std::filesystem::exists(path, error) ? std::filesystem::file_size(path, error) : 0;
    if (error) return AppendResult::kIo;
    record["previous_hash"] = tail_hash_;
    record["segment"] = segment_index_;
    record["hash"] = Sha256(record.dump());
    std::string line = record.dump() + "\n";
    if (current > 0 && (current >= config_.rotation_size_bytes ||
                        line.size() > config_.rotation_size_bytes - current)) {
      path = config_.directory / SegmentName(++segment_index_);
      record["segment"] = segment_index_;
      record.erase("hash");
      record["hash"] = Sha256(record.dump());
      line = record.dump() + "\n";
    }
    if (line.size() > config_.rotation_size_bytes) return AppendResult::kResourceExhausted;
    if (!WriteSecure(path, line, O_APPEND) || !SyncDirectory(config_.directory))
      return AppendResult::kIo;
    tail_hash_ = record.at("hash").get<std::string>();
    records_.push_back(std::move(record));
    ++record_count_;
    if (!Prune() || !WriteState()) return AppendResult::kIo;
    return AppendResult::kOk;
  }

  bool Prune() {
    bool listed = false;
    auto segments = Segments(&listed);
    if (!listed) return false;
    while (segments.size() > config_.rotation_files) {
      const std::filesystem::path removed = segments.front().second;
      std::error_code error;
      std::filesystem::remove(removed, error);
      if (error) return false;
      if (!SyncDirectory(config_.directory)) return false;
      segments.erase(segments.begin());
    }
    if (segments.empty()) return true;
    const std::uint64_t first = segments.front().first;
    records_.erase(std::remove_if(records_.begin(), records_.end(),
                                  [first](const json& record) {
                                    return record.value("segment", first) < first;
                                  }),
                   records_.end());
    record_count_ = records_.size();
    return true;
  }

  bool AppendRecord(json record) {
    const AppendResult result = Append(std::move(record));
    if (result != AppendResult::kOk) {
      if (result == AppendResult::kLockLost)
        Degrade(ErrorCode::kWriterConflict, "action_list_audit_writer_conflict", "append_lock_identity",
                segment_index_, record_count_ + 1);
      else if (result == AppendResult::kResourceExhausted)
        Degrade(ErrorCode::kResourceExhausted, "action_list_audit_record_too_large");
      else
        Degrade(ErrorCode::kIo, "action_list_audit_write_failed");
      stop_.store(true);
      return false;
    }
    return true;
  }

  bool Snapshot() {
    Proto::GetRuntimeSnapshotRequest request;
    Proto::ManagerRuntimeSnapshot snapshot;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    const eos::lib::CorrelationScope correlation;
    context.AddMetadata(eos::lib::kRequestIdMetadataKey, correlation.id());
    SetActive(&context);
    const grpc::Status result =
        client_.workflows().GetRuntimeSnapshot(&context, request, &snapshot);
    ClearActive(&context);
    if (!result.ok()) {
      if (!(stop_.load() && result.error_code() == grpc::StatusCode::CANCELLED)) {
        eos::lib::LogEvent(Logger(), spdlog::level::warn, "action_list_audit_snapshot_retry",
                           {{"grpc_status", static_cast<int>(result.error_code())}});
      }
      return false;
    }
    if (snapshot.action_list_cursor().epoch().empty()) {
      eos::lib::LogEvent(
          Logger(), spdlog::level::warn, "action_list_audit_snapshot_retry",
          {{"grpc_status", static_cast<int>(grpc::StatusCode::OK)}, {"reason", "invalid_cursor"}});
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& cursor = snapshot.action_list_cursor();
    json action_lists = json::array();
    for (const auto& list : snapshot.action_lists()) {
      json projected = ActionListRecord(list, cursor, snapshot.captured_at());
      projected.erase("kind");
      projected.erase("cursor");
      projected.erase("observed_at");
      action_lists.push_back(std::move(projected));
    }
    json facts = json::array();
    for (const auto& fact : snapshot.facts()) {
      json projected = FactRecord(fact, cursor, snapshot.captured_at());
      projected.erase("kind");
      projected.erase("cursor");
      projected.erase("observed_at");
      facts.push_back(std::move(projected));
    }
    json commit{{"kind", "snapshot_commit"},
                {"cursor", CursorFields(cursor)},
                {"observed_at", Timestamp(snapshot.captured_at())},
                {"action_lists", std::move(action_lists)},
                {"facts", std::move(facts)}};
    if (!AppendRecord(std::move(commit))) return false;
    epoch_ = cursor.epoch();
    sequence_ = cursor.sequence();
    if (!WriteState()) {
      Degrade(ErrorCode::kIo, "action_list_audit_state_write_failed");
      return false;
    }
    health_ = Health::kHealthy;
    return true;
  }

  bool Consume(const Proto::ActionListEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (event.cursor().epoch() == epoch_ && event.cursor().sequence() <= sequence_) return true;
    if (event.cursor().epoch() != epoch_ || event.cursor().sequence() != sequence_ + 1)
      return false;
    json record;
    if (event.has_action_list())
      record = ActionListRecord(event.action_list(), event.cursor(), event.observed_at());
    else if (event.has_fact())
      record = FactRecord(event.fact(), event.cursor(), event.observed_at());
    else
      return false;
    if (!AppendRecord(std::move(record))) return false;
    epoch_ = event.cursor().epoch();
    sequence_ = event.cursor().sequence();
    if (!WriteState()) {
      Degrade(ErrorCode::kIo, "action_list_audit_state_write_failed");
      stop_.store(true);
      return false;
    }
    health_ = Health::kHealthy;
    return true;
  }

  void Run() {
    while (!stop_.load()) {
      bool need_snapshot = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        need_snapshot = epoch_.empty();
      }
      if (need_snapshot && !Snapshot()) {
        Retry();
        continue;
      }
      Proto::WatchActionListEventsRequest request;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        request.mutable_after()->set_epoch(epoch_);
        request.mutable_after()->set_sequence(sequence_);
      }
      grpc::ClientContext context;
      const eos::lib::CorrelationScope correlation;
      context.AddMetadata(eos::lib::kRequestIdMetadataKey, correlation.id());
      SetActive(&context);
      std::unique_ptr<grpc::ClientReader<Proto::ActionListEvent>> reader =
          client_.workflows().WatchActionListEvents(&context, request);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (health_ != Health::kDegraded) health_ = Health::kHealthy;
      }
      Proto::ActionListEvent event;
      bool gap = false;
      while (!stop_.load() && reader->Read(&event)) {
        if (!Consume(event)) {
          gap = true;
          context.TryCancel();
          break;
        }
      }
      const grpc::Status result = reader->Finish();
      ClearActive(&context);
      if (stop_.load()) break;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (health_ != Health::kDegraded) health_ = Health::kStopped;
      }
      if (gap || result.error_code() == grpc::StatusCode::FAILED_PRECONDITION ||
          result.error_code() == grpc::StatusCode::OUT_OF_RANGE) {
        std::lock_guard<std::mutex> lock(mutex_);
        epoch_.clear();
        sequence_ = 0;
        if (!WriteState()) {
          Degrade(ErrorCode::kIo, "action_list_audit_state_write_failed");
          stop_.store(true);
          continue;
        }
        eos::lib::LogEvent(Logger(), spdlog::level::warn, "action_list_audit_resnapshot_required",
                           {{"grpc_status", static_cast<int>(result.error_code())}});
      } else {
        eos::lib::LogEvent(Logger(), spdlog::level::warn, "action_list_audit_stream_retry",
                           {{"grpc_status", static_cast<int>(result.error_code())}});
      }
      Retry();
    }
  }

  void Retry() const {
    const auto deadline = std::chrono::steady_clock::now() + config_.reconnect_delay;
    while (!stop_.load() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  void SetActive(grpc::ClientContext* context) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_context_ = context;
    if (stop_.load()) context->TryCancel();
  }

  void ClearActive(grpc::ClientContext* context) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_context_ == context) active_context_ = nullptr;
  }

  Client& client_;
  Config config_;
  mutable std::mutex mutex_;
  std::atomic_bool stop_{false};
  std::thread worker_;
  grpc::ClientContext* active_context_ = nullptr;
  Health health_ = Health::kStopped;
  ErrorCode error_ = ErrorCode::kNone;
  bool running_ = false;
  int lock_fd_ = -1;
  dev_t lock_device_ = 0;
  ino_t lock_inode_ = 0;
  std::string epoch_;
  std::uint64_t sequence_ = 0;
  std::uint64_t record_count_ = 0;
  std::uint64_t segment_index_ = 0;
  std::string tail_hash_ = kZeroHash;
  std::vector<json> records_;
};

Service::Service(Client& client, Config config)
    : impl_(std::make_unique<Impl>(client, std::move(config))) {}

Service::~Service() = default;

void Service::Start() { impl_->Start(); }

void Service::Stop() noexcept { impl_->Stop(); }

Status Service::status() const { return impl_->GetStatus(); }

RecordPage Service::record_page(std::size_t limit, std::size_t maximum_encoded_bytes) const {
  return impl_->Page(limit, maximum_encoded_bytes);
}

const char* ToString(Health value) noexcept {
  switch (value) {
    case Health::kStopped:
      return "stopped";
    case Health::kHealthy:
      return "healthy";
    case Health::kDegraded:
      return "degraded";
  }
  return "degraded";
}

const char* ToString(ErrorCode value) noexcept {
  switch (value) {
    case ErrorCode::kNone:
      return "none";
    case ErrorCode::kInvalidConfig:
      return "invalid_config";
    case ErrorCode::kIo:
      return "io";
    case ErrorCode::kWriterConflict:
      return "writer_conflict";
    case ErrorCode::kCorruptChain:
      return "corrupt_chain";
    case ErrorCode::kResourceExhausted:
      return "resource_exhausted";
  }
  return "corrupt_chain";
}

}  // namespace eos::ext::north_http::audit
