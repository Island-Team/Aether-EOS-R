/// Profile-neutral admitted skill provider contract.
#pragma once

#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace eos::runtime::skills {

// Canonical timing metadata carried by data exchanged through the skill
// runtime. source_timestamp_ns is meaningful only in source_clock_id; EOS
// never converts an unknown device clock into wall time.
struct EventTime {
  std::optional<std::int64_t> source_timestamp_ns;
  std::string source_clock_id;
  std::chrono::system_clock::time_point observed_at{};
  std::int64_t observed_monotonic_ns{0};
  std::string boot_id;
  std::uint64_t sequence{0};
  std::string event_id;
};

struct PrimitiveReference {
  std::string name;
  std::string version;
  bool operator==(const PrimitiveReference& other) const {
    return name == other.name && version == other.version;
  }
};

struct Resource {
  std::string uri_template;
  std::string description;
  bool operator==(const Resource& other) const {
    return uri_template == other.uri_template && description == other.description;
  }
};

enum class ClaimKind { kDevice, kResource, kProviderContext };
enum class ClaimMode { kSharedRead, kExclusiveUse };

struct SignalDescriptor {
  std::string name;
  nlohmann::json schema = nlohmann::json::object();
};

enum class SourceAuthority { kRead, kWrite };
struct SourceDescriptor {
  std::string topic;
  nlohmann::json schema = nlohmann::json::object();
  SourceAuthority authority{SourceAuthority::kRead};
};
enum class SourceLookup { kLatest, kExact, kBefore, kAfter, kNearest };
struct SourceSample {
  std::string topic;
  std::int64_t source_timestamp_ns{0};
  std::uint64_t sequence{0};
  nlohmann::json value = nlohmann::json::object();
  EventTime event_time;
};

enum class InferenceAuthority { kVad, kHuman, kHumanPose, kFaceRecognition, kChatCompletion, kVlm };
struct InferenceRequest {
  InferenceAuthority authority{InferenceAuthority::kHuman};
  std::string source;
  std::uint64_t source_sequence{0};
  std::uint64_t source_generation{0};
  nlohmann::json options = nlohmann::json::object();
};
struct InferenceResult {
  InferenceAuthority authority{InferenceAuthority::kHuman};
  std::string source;
  std::uint64_t source_sequence{0};
  std::uint64_t source_generation{0};
  nlohmann::json value = nlohmann::json::object();
};

struct IpcChannelDescriptor {
  std::string name;
  nlohmann::json schema = nlohmann::json::object();
  bool can_send{false};
  bool can_receive{false};
};

struct ProcessHandle {
  std::string execution_id;
  std::string parent_execution_id;
  std::uint64_t generation{0};
};
struct ProcessExit {
  std::string status;
  std::string reason_code;
  nlohmann::json result = nlohmann::json::object();
  std::string message;
};

struct IpcMessage {
  std::string message_id;
  std::string request_id;
  std::string correlation_id;
  std::string causation_id;
  std::string sender_execution_id;
  std::string recipient_execution_id;
  std::string channel;
  std::uint32_t schema_version{1};
  nlohmann::json payload = nlohmann::json::object();
  EventTime event_time;
  std::optional<std::chrono::system_clock::time_point> deadline;
};

struct RetainedExecutionState {
  std::uint64_t revision{0};
  std::chrono::system_clock::time_point published_at{};
  nlohmann::json payload = nlohmann::json::object();
};

struct ClaimTemplate {
  ClaimKind kind{ClaimKind::kResource};
  ClaimMode mode{ClaimMode::kExclusiveUse};
  std::string argument_pointer;
};

struct Metadata {
  std::string name;
  std::string version;
  std::string description;
  std::string library;
  std::string library_version;
  std::string build_id;
  nlohmann::json parameters_schema;
  nlohmann::json returns_schema;
  std::optional<nlohmann::json> execution_state_schema;
  std::optional<nlohmann::json> related_process_state_schema;
  std::vector<PrimitiveReference> primitives;
  std::vector<Resource> resources;
  std::vector<SignalDescriptor> accepted_signals;
  std::vector<SignalDescriptor> emitted_signals;
  std::vector<ClaimTemplate> claim_templates;
  std::vector<SourceDescriptor> sources;
  std::vector<InferenceAuthority> inference_authorities;
  std::vector<IpcChannelDescriptor> ipc_channels;
  bool supports_signals{false};
  bool reentrant{false};
};

using Cancelled = std::function<bool()>;

struct Signal {
  std::string name;
  nlohmann::json payload = nlohmann::json::object();
  std::string signal_id;
  std::string execution_id;
};

struct Checkpoint {
  std::string name;
  std::string message;
  std::optional<double> progress;
  nlohmann::json fields = nlohmann::json::object();
};

struct Activity {
  std::string name;
  nlohmann::json fields = nlohmann::json::object();
};

struct ExecutionHooks {
  std::string execution_id;
  std::string runtime_id;
  std::optional<ProcessHandle> parent_process;
  Cancelled cancelled;
  std::function<std::optional<Signal>(std::chrono::milliseconds)> receive_signal;
  std::function<bool(const Signal&)> emit_signal;
  std::function<bool(const Checkpoint&)> emit_checkpoint;
  std::function<std::string(const Activity&)> begin_activity;
  std::function<void(const std::string&, const std::string&)> end_activity;
  std::function<void(const std::string&, std::chrono::milliseconds)> record_capability;
  std::function<void(bool)> diagnostic_log;
  std::function<bool(nlohmann::json)> publish_execution_state;
  std::function<bool(nlohmann::json)> publish_related_process_state;
  std::function<std::optional<RetainedExecutionState>(const ProcessHandle&)>
      read_related_process_state;
  std::function<std::optional<RetainedExecutionState>(const ProcessHandle&, std::uint64_t,
                                                        std::chrono::milliseconds)>
      wait_related_process_state;
  // Returns a release callback after the device is available. An empty callback
  // means the execution was cancelled while waiting for the device.
  std::function<std::function<void()>(const std::string&)> acquire_device;
  std::function<SourceSample(const std::string&, std::int64_t, nlohmann::json)> publish_source;
  std::function<std::optional<SourceSample>(const std::string&, SourceLookup, std::int64_t)>
      query_source;
  std::function<std::optional<SourceSample>(const std::string&, std::uint64_t,
                                            std::chrono::milliseconds)> wait_source;
  std::function<ProcessHandle(const std::string&, const std::string&, nlohmann::json,
                              const std::string&, std::chrono::milliseconds)> spawn;
  std::function<bool(const ProcessHandle&, const std::string&, nlohmann::json,
                     const std::string&, std::chrono::milliseconds)> send_message;
  std::function<std::optional<IpcMessage>(const std::string&, std::chrono::milliseconds)>
      receive_message;
  std::function<std::optional<IpcMessage>(const ProcessHandle&, const std::string&,
                                           const std::string&, std::chrono::milliseconds)>
      receive_reply;
  std::function<std::optional<ProcessExit>(const ProcessHandle&, std::chrono::milliseconds)>
      await_exit;
  std::function<bool(const ProcessHandle&)> terminate_process;
};

struct RunResult {
  enum class Status {
    kCompleted,
    kInvalidArgument,
    kCancelled,
    kFailed,
    kProtocolError,
    kInternal,
  };

  Status status{Status::kFailed};
  bool accepted{false};
  std::string reason_code;
  nlohmann::json result = nlohmann::json::object();
  std::vector<nlohmann::json> primitive_acks;
  std::string message;
};

using RunCompletion = std::function<void(RunResult)>;

class Provider {
 public:
  virtual ~Provider() = default;
  virtual std::vector<Metadata> Discover() const = 0;
  virtual const Metadata* Find(const std::string& name, const std::string& version) const = 0;
  virtual void ValidateArguments(const std::string& name, const std::string& version,
                                 const nlohmann::json& arguments) const = 0;
  virtual RunResult Execute(const std::string& name, const std::string& version,
                            const nlohmann::json& arguments, const std::string& command_id,
                            Cancelled cancelled = {}) const = 0;
  virtual RunResult ExecuteWithHooks(const std::string& name, const std::string& version,
                                     const nlohmann::json& arguments, const std::string& command_id,
                                     ExecutionHooks hooks) const {
    return Execute(name, version, arguments, command_id, std::move(hooks.cancelled));
  }
  /// Starts an execution and invokes completion exactly once. Providers whose
  /// implementation is synchronous inherit this compatibility adapter.
  virtual void ExecuteAsync(const std::string& name, const std::string& version,
                            const nlohmann::json& arguments, const std::string& command_id,
                            ExecutionHooks hooks, RunCompletion completion) const {
    completion(ExecuteWithHooks(name, version, arguments, command_id, std::move(hooks)));
  }
};

}  // namespace eos::runtime::skills
