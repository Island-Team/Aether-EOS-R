#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

#include "common.h"
#include "dispatcher.h"
#include "src/eos/lib/log.h"

namespace eos::runtime::dispatch {
namespace {

constexpr std::size_t kMaximumCommandIdBytes = 128;
constexpr auto kAdmissionCancellationPoll = std::chrono::milliseconds(10);

bool BypassesDeviceAdmission(const std::string& primitive) {
  return primitive == "safety.estop" || primitive == "robot.stop" || primitive == "motion.stop";
}

std::string Fingerprint(const std::string& primitive, const std::string& version,
                        const std::string& device_id, nlohmann::json arguments) {
  arguments.erase("command_id");
  return nlohmann::json{{"primitive", primitive},
                        {"version", version},
                        {"device_id", device_id},
                        {"arguments", std::move(arguments)}}
      .dump();
}

nlohmann::json RecoveredUnknown(const std::string& primitive, const std::string& version,
                                const std::string& command_id, std::uint64_t sequence) {
  return {{"type", "primitive.ack"},
          {"primitive", primitive},
          {"primitive_version", version},
          {"accepted", true},
          {"reason", nullptr},
          {"reason_code", nullptr},
          {"command_id", command_id},
          {"sequence", sequence},
          {"result",
           {{"status", "outcome_unknown"},
            {"reason", "runtime restarted before a terminal backend result was recorded"}}}};
}

}  // namespace

bool Dispatcher::IsCancellable(const std::string& primitive) {
  return primitive == "base.velocity" || primitive == "base.navigate" ||
         primitive == "base.posture" || primitive == "gimbal.motion" || primitive == "arm.init" ||
         primitive == "arm.movej" || primitive == "arm.movel" || primitive == "arm.pick" ||
         primitive == "gripper.motion";
}

void Dispatcher::AppendCommandJournal(const nlohmann::json& entry) const {
  if (command_journal_.empty()) return;
  const std::string line = entry.dump() + "\n";
  const int descriptor =
      ::open(command_journal_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    throw std::runtime_error("cannot open primitive command journal '" + command_journal_.string() +
                             "': " + std::strerror(errno));
  }
  std::size_t offset = 0;
  while (offset < line.size()) {
    const ssize_t written = ::write(descriptor, line.data() + offset, line.size() - offset);
    if (written <= 0) {
      const std::string detail = std::strerror(errno);
      ::close(descriptor);
      throw std::runtime_error("cannot write primitive command journal: " + detail);
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    const std::string detail = std::strerror(errno);
    ::close(descriptor);
    throw std::runtime_error("cannot sync primitive command journal: " + detail);
  }
  if (::close(descriptor) != 0) {
    throw std::runtime_error("cannot close primitive command journal: " +
                             std::string(std::strerror(errno)));
  }
}

void Dispatcher::LoadCommandJournal() {
  if (command_journal_.empty() || !std::filesystem::exists(command_journal_)) return;
  std::ifstream input(command_journal_);
  if (!input) {
    throw std::runtime_error("cannot read primitive command journal '" + command_journal_.string() +
                             "'");
  }
  std::string line;
  std::size_t line_number = 0;
  std::uint64_t maximum_sequence = ack_sequence_.load();
  while (std::getline(input, line)) {
    ++line_number;
    if (line.empty()) continue;
    nlohmann::json entry;
    try {
      entry = nlohmann::json::parse(line);
    } catch (const std::exception& error) {
      throw std::runtime_error("invalid primitive command journal line " +
                               std::to_string(line_number) + ": " + error.what());
    }
    const std::string type = entry.value("type", "");
    const std::string command_id = entry.value("command_id", "");
    if (command_id.empty()) {
      throw std::runtime_error("primitive command journal line " + std::to_string(line_number) +
                               " has no command_id");
    }
    if (type == "prepared") {
      auto record = std::make_shared<CommandRecord>();
      record->fingerprint = entry.at("fingerprint").get<std::string>();
      record->primitive = entry.at("primitive").get<std::string>();
      record->version = entry.at("version").get<std::string>();
      record->device_id = entry.at("device_id").get<std::string>();
      record->cancellable = entry.value("cancellable", false);
      commands_[command_id] = std::move(record);
    } else if (type == "terminal") {
      const auto found = commands_.find(command_id);
      if (found == commands_.end()) {
        throw std::runtime_error("primitive command journal terminal without reservation at line " +
                                 std::to_string(line_number));
      }
      found->second->result.status =
          static_cast<DispatchResult::Status>(entry.at("status").get<int>());
      found->second->result.payload = entry.at("payload");
      found->second->result.message = entry.value("message", "");
      found->second->complete = true;
      if (found->second->result.payload.is_object()) {
        maximum_sequence = std::max(
            maximum_sequence, found->second->result.payload.value<std::uint64_t>("sequence", 0));
      }
    } else {
      throw std::runtime_error("unknown primitive command journal entry at line " +
                               std::to_string(line_number));
    }
  }
  if (!input.eof()) throw std::runtime_error("cannot finish reading primitive command journal");
  ack_sequence_.store(maximum_sequence);

  for (const auto& [command_id, record] : commands_) {
    if (record->complete) continue;
    record->result = {
        DispatchResult::Status::kCompleted,
        RecoveredUnknown(record->primitive, record->version, command_id, ++ack_sequence_),
        {}};
    record->complete = true;
    AppendCommandJournal({{"type", "terminal"},
                          {"command_id", command_id},
                          {"status", static_cast<int>(record->result.status)},
                          {"payload", record->result.payload},
                          {"message", record->result.message}});
  }
}

DispatchResult Dispatcher::ExecuteCommand(const std::string& primitive, const std::string& version,
                                          const std::string& device_id, nlohmann::json arguments,
                                          const std::string& command_id,
                                          const std::function<bool()>& cancelled) {
  if (command_id.empty() || command_id.size() > kMaximumCommandIdBytes) {
    return {DispatchResult::Status::kInvalidArgument, nullptr,
            "command_id must contain 1..128 bytes"};
  }
  if (!arguments.is_object()) {
    return {DispatchResult::Status::kInvalidArgument, nullptr, "arguments must be an object"};
  }
  const auto embedded = arguments.find("command_id");
  if (embedded != arguments.end() && !embedded->is_null() &&
      (!embedded->is_string() || embedded->get<std::string>() != command_id)) {
    return {DispatchResult::Status::kInvalidArgument, nullptr,
            "arguments.command_id conflicts with command_id"};
  }
  arguments["command_id"] = command_id;
  if (!arguments.contains("type")) arguments["type"] = primitive;
  const std::string fingerprint = Fingerprint(primitive, version, device_id, arguments);

  std::shared_ptr<CommandRecord> record;
  bool uses_device_admission = false;
  bool admission_full = false;
  std::size_t admission_depth = 0;
  const auto admission_started = std::chrono::steady_clock::now();
  {
    std::unique_lock<std::mutex> lock(command_mutex_);
    const auto found = commands_.find(command_id);
    if (found != commands_.end()) {
      record = found->second;
      if (record->fingerprint != fingerprint) {
        return {DispatchResult::Status::kAlreadyExists, nullptr,
                "command_id is already reserved for a different primitive request"};
      }
      record->changed.wait(lock, [&] { return record->complete; });
      return record->result;
    }
    record = std::make_shared<CommandRecord>();
    record->fingerprint = fingerprint;
    record->primitive = primitive;
    record->version = version;
    record->device_id = device_id;
    record->cancellable = IsCancellable(primitive);
    AppendCommandJournal({{"type", "prepared"},
                          {"command_id", command_id},
                          {"fingerprint", fingerprint},
                          {"primitive", primitive},
                          {"version", version},
                          {"device_id", device_id},
                          {"cancellable", record->cancellable}});
    commands_.emplace(command_id, record);
    uses_device_admission = !device_id.empty() && DeviceMutex(device_id) != nullptr &&
                            !BypassesDeviceAdmission(primitive);
    if (uses_device_admission) {
      auto& queue = device_admission_[device_id];
      if (queue.size() >= admission_.max_commands_per_device) {
        admission_full = true;
      } else {
        queue.push_back(command_id);
        admission_depth = queue.size();
      }
    }
  }
  if (admission_full) {
    lib::LogEvent(lib::GetLogger("dispatch"), spdlog::level::warn, "primitive_admission_rejected",
                  {{"primitive", primitive},
                   {"device_id", device_id},
                   {"command_id", command_id},
                   {"reason", "queue_full"},
                   {"limit", admission_.max_commands_per_device}});
  } else if (admission_depth > 1) {
    lib::LogEvent(lib::GetLogger("dispatch"), spdlog::level::debug, "primitive_admission_queued",
                  {{"primitive", primitive},
                   {"device_id", device_id},
                   {"command_id", command_id},
                   {"queue_depth", admission_depth}});
  }

  DispatchResult result;
  bool owns_admission = false;
  if (admission_full) {
    result = {DispatchResult::Status::kCompleted,
              Ack(primitive, version, arguments, false, detail::kBusy,
                  "device command admission queue is full for '" + device_id + "'"),
              {}};
    result.payload["result"] = {{"status", "rejected"}, {"reason", "admission queue is full"}};
  } else if (uses_device_admission) {
    const auto deadline = std::chrono::steady_clock::now() + admission_.max_wait;
    bool wait_cancelled = false;
    bool wait_timed_out = false;
    while (true) {
      {
        std::unique_lock<std::mutex> lock(command_mutex_);
        const auto queue = device_admission_.find(device_id);
        if (queue != device_admission_.end() && !queue->second.empty() &&
            queue->second.front() == command_id) {
          owns_admission = true;
          break;
        }
        if (record->cancel_requested) {
          wait_cancelled = true;
          break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          wait_timed_out = true;
          break;
        }
        admission_changed_.wait_for(lock, kAdmissionCancellationPoll);
      }
      if (cancelled && cancelled()) {
        std::lock_guard<std::mutex> lock(command_mutex_);
        record->cancel_requested = true;
        wait_cancelled = true;
        break;
      }
    }
    if (wait_cancelled) {
      result = {DispatchResult::Status::kCompleted,
                Ack(primitive, version, arguments, false, detail::kBusy,
                    "command was cancelled while waiting for device admission"),
                {}};
      result.payload["result"] = {{"status", "cancelled"}};
      lib::LogEvent(lib::GetLogger("dispatch"), spdlog::level::debug,
                    "primitive_admission_cancelled",
                    {{"primitive", primitive},
                     {"device_id", device_id},
                     {"command_id", command_id},
                     {"wait_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - admission_started)
                                     .count()}});
    } else if (wait_timed_out) {
      result = {DispatchResult::Status::kCompleted,
                Ack(primitive, version, arguments, false, detail::kBusy,
                    "device admission deadline expired before backend release"),
                {}};
      result.payload["result"] = {{"status", "timed_out"},
                                  {"reason", "device admission deadline expired"}};
      lib::LogEvent(lib::GetLogger("dispatch"), spdlog::level::warn,
                    "primitive_admission_timed_out",
                    {{"primitive", primitive},
                     {"device_id", device_id},
                     {"command_id", command_id},
                     {"wait_ms", admission_.max_wait.count()}});
    } else if (owns_admission && admission_depth > 1) {
      lib::LogEvent(lib::GetLogger("dispatch"), spdlog::level::debug,
                    "primitive_admission_acquired",
                    {{"primitive", primitive},
                     {"device_id", device_id},
                     {"command_id", command_id},
                     {"wait_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - admission_started)
                                     .count()}});
    }
  }
  if (!admission_full && (!uses_device_admission || owns_admission)) {
    auto mark_backend_started = [this, record, command_id] {
      bool release_cancel = false;
      {
        std::lock_guard<std::mutex> lock(command_mutex_);
        record->backend_started = true;
        if (record->cancel_requested && !record->cancel_released) {
          record->cancel_released = true;
          release_cancel = true;
        }
      }
      if (!release_cancel || record->device_id.empty()) return;
      if (hardware_runtime::Device* device = registry_.FindDeviceForRelease(record->device_id)) {
        try {
          device->backend().CancelCommand(command_id);
        } catch (...) {
          lib::LogEvent(lib::GetLogger("dispatch"), spdlog::level::err,
                        "primitive_backend_cancel_failed",
                        {{"primitive", record->primitive},
                         {"device_id", record->device_id},
                         {"command_id", command_id},
                         {"reason", "exception"}});
        }
      }
      if (record->primitive == "base.velocity") {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        base_velocity_lease_s_.erase(record->device_id);
      }
    };
    result = ExecuteTerminal(
        primitive, version, device_id, std::move(arguments),
        [record, cancelled] { return record->cancel_requested || (cancelled && cancelled()); },
        mark_backend_started);
  }
  try {
    AppendCommandJournal({{"type", "terminal"},
                          {"command_id", command_id},
                          {"status", static_cast<int>(result.status)},
                          {"payload", result.payload},
                          {"message", result.message}});
  } catch (const std::exception& error) {
    result.status = DispatchResult::Status::kCompleted;
    result.payload = RecoveredUnknown(primitive, version, command_id, ++ack_sequence_);
    result.payload["result"]["reason"] = error.what();
    result.message.clear();
  }
  {
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (uses_device_admission && !admission_full) {
      const auto queue = device_admission_.find(device_id);
      if (queue != device_admission_.end()) {
        const auto command = std::find(queue->second.begin(), queue->second.end(), command_id);
        if (command != queue->second.end()) queue->second.erase(command);
        if (queue->second.empty()) device_admission_.erase(queue);
      }
    }
    record->result = result;
    record->complete = true;
  }
  admission_changed_.notify_all();
  record->changed.notify_all();
  return result;
}

DispatchResult Dispatcher::CancelCommand(const std::string& command_id) {
  if (command_id.empty()) {
    return {DispatchResult::Status::kInvalidArgument, nullptr, "command_id is required"};
  }
  std::shared_ptr<CommandRecord> record;
  bool release_cancel = false;
  {
    std::lock_guard<std::mutex> lock(command_mutex_);
    const auto found = commands_.find(command_id);
    if (found == commands_.end()) {
      return {DispatchResult::Status::kNotFound, nullptr, "primitive command is not found"};
    }
    record = found->second;
    if (!record->cancellable) {
      return {DispatchResult::Status::kFailedPrecondition, nullptr,
              "primitive command is not cancellable"};
    }
    if (record->complete) return record->result;
    record->cancel_requested = true;
    if (record->backend_started && !record->cancel_released) {
      record->cancel_released = true;
      release_cancel = true;
    }
  }
  admission_changed_.notify_all();
  if (release_cancel && !record->device_id.empty()) {
    if (hardware_runtime::Device* device = registry_.FindDeviceForRelease(record->device_id)) {
      try {
        device->backend().CancelCommand(command_id);
      } catch (...) {
        lib::LogEvent(lib::GetLogger("dispatch"), spdlog::level::err,
                      "primitive_backend_cancel_failed",
                      {{"primitive", record->primitive},
                       {"device_id", record->device_id},
                       {"command_id", command_id},
                       {"reason", "exception"}});
      }
    }
    if (record->primitive == "base.velocity") {
      std::lock_guard<std::mutex> state_lock(state_mutex_);
      base_velocity_lease_s_.erase(record->device_id);
    }
  }
  std::unique_lock<std::mutex> lock(command_mutex_);
  record->changed.wait(lock, [&] { return record->complete; });
  return record->result;
}

NavigationCancelStatus Dispatcher::CancelActiveNavigationCommand(
    const std::string& command_id, const std::string& device_id) {
  if (command_id.empty() || device_id.empty()) return NavigationCancelStatus::kNotFound;
  std::shared_ptr<CommandRecord> record;
  bool release_cancel = false;
  {
    std::lock_guard<std::mutex> lock(command_mutex_);
    const auto found = commands_.find(command_id);
    if (found == commands_.end()) return NavigationCancelStatus::kNotFound;
    record = found->second;
    if (record->primitive != "base.navigate" || record->device_id != device_id)
      return NavigationCancelStatus::kMismatch;
    if (record->complete) return NavigationCancelStatus::kNotFound;
    record->cancel_requested = true;
    if (record->backend_started && !record->cancel_released) {
      record->cancel_released = true;
      release_cancel = true;
    }
  }
  admission_changed_.notify_all();
  bool backend_accepted = false;
  if (release_cancel) {
    if (hardware_runtime::Device* device = registry_.FindDeviceForRelease(device_id)) {
      try {
        backend_accepted =
            device->backend().CancelCommand(command_id) == hardware::ReleaseResult::kAccepted;
      } catch (...) {
        lib::LogEvent(lib::GetLogger("dispatch"), spdlog::level::err,
                      "primitive_backend_cancel_failed",
                      {{"primitive", "base.navigate"},
                       {"device_id", device_id},
                       {"command_id", command_id},
                       {"reason", "exception"}});
      }
    }
  }
  std::unique_lock<std::mutex> lock(command_mutex_);
  record->changed.wait(lock, [&] { return record->complete; });
  const auto& payload = record->result.payload;
  const bool terminal_cancelled =
      record->result.status == DispatchResult::Status::kCompleted && payload.is_object() &&
      payload.contains("result") && payload.at("result").is_object() &&
      payload.at("result").value("status", std::string{}) == "cancelled";
  // A queued command has not reached the backend, so its cancelled terminal is definitive.
  if (!record->backend_started && terminal_cancelled)
    return NavigationCancelStatus::kCancelled;
  // The backend may report its own nonzero terminal code for a cancelled goal.
  // Acceptance of the targeted stop plus a settled terminal boundary is enough.
  if (release_cancel && backend_accepted &&
      record->result.status == DispatchResult::Status::kCompleted && payload.is_object() &&
      payload.contains("result") && payload.at("result").is_object() &&
      payload.at("result").value("status", std::string{}) !=
          "outcome_unknown")
    return NavigationCancelStatus::kCancelled;
  return NavigationCancelStatus::kUncertain;
}

}  // namespace eos::runtime::dispatch
