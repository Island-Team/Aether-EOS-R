#include "adapters/eos-cxx/runtime/skills/worker/protocol.h"
#include "adapters/eos-cxx/runtime/skills/worker/sandbox.h"
#include "adapters/eos-cxx/runtime/skills/native/runtime.h"

#include <nlohmann/json.hpp>
#include <unistd.h>

#include <atomic>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>

namespace sw = eos::runtime::skills::worker;
namespace ns = eos::runtime::skills::native;
namespace skills = eos::runtime::skills;
using json = nlohmann::json;
namespace {
constexpr int kControlFd = 3;
std::uint64_t next_request_id = 1;
std::mutex broker_mutex;
std::uint64_t current_generation = 0;
std::string current_execution_id;
struct WorkerGrant { std::string token; std::string device_id; };
std::map<std::tuple<std::string, std::string, std::string>, WorkerGrant> grants;

json BrokerRequest(json request) {
  std::lock_guard lock(broker_mutex);
  const std::uint64_t id = next_request_id++;
  request["execution_id"] = current_execution_id;
  sw::Send(kControlFd, {sw::MessageType::kCapabilityCall, id, current_generation,
                       request.dump()});
  auto reply = sw::Receive(kControlFd);
  if (!reply || reply->type != sw::MessageType::kCapabilityReply || reply->request_id != id ||
      reply->generation != current_generation)
    throw std::runtime_error("worker broker reply unavailable");
  return json::parse(reply->payload);
}

json ToJson(const skills::Metadata& m) {
  json primitives = json::array();
  for (const auto& p : m.primitives) primitives.push_back({{"name", p.name}, {"version", p.version}});
  json resources = json::array();
  for (const auto& r : m.resources)
    resources.push_back({{"uri_template", r.uri_template}, {"description", r.description}});
  json claims = json::array();
  for (const auto& c : m.claim_templates)
    claims.push_back({{"kind", static_cast<int>(c.kind)}, {"mode", static_cast<int>(c.mode)},
                      {"argument_pointer", c.argument_pointer}});
  json accepted = json::array(), emitted = json::array();
  for (const auto& signal : m.accepted_signals)
    accepted.push_back({{"name", signal.name}, {"schema", signal.schema}});
  for (const auto& signal : m.emitted_signals)
    emitted.push_back({{"name", signal.name}, {"schema", signal.schema}});
  json sources = json::array();
  for (const auto& source : m.sources)
    sources.push_back({{"topic", source.topic}, {"schema", source.schema},
                       {"authority", static_cast<int>(source.authority)}});
  json inference = json::array();
  for (const auto authority : m.inference_authorities)
    inference.push_back(static_cast<int>(authority));
  json channels = json::array();
  for (const auto& channel : m.ipc_channels)
    channels.push_back({{"name", channel.name}, {"schema", channel.schema},
                        {"can_send", channel.can_send}, {"can_receive", channel.can_receive}});
  return {{"name", m.name}, {"version", m.version}, {"description", m.description},
          {"library", m.library}, {"library_version", m.library_version},
          {"build_id", m.build_id}, {"parameters_schema", m.parameters_schema},
          {"returns_schema", m.returns_schema}, {"primitives", primitives},
          {"resources", resources}, {"claims", claims},
          {"execution_state_schema", m.execution_state_schema.value_or(json())},
          {"related_process_state_schema", m.related_process_state_schema.value_or(json())},
          {"accepted_signals", accepted}, {"emitted_signals", emitted},
          {"sources", sources}, {"inference_authorities", inference},
          {"ipc_channels", channels},
          {"supports_signals", m.supports_signals}, {"reentrant", m.reentrant}};
}

json ToJson(const skills::RunResult& result) {
  return {{"status", static_cast<int>(result.status)}, {"accepted", result.accepted},
          {"reason_code", result.reason_code}, {"result", result.result},
          {"primitive_acks", result.primitive_acks}, {"message", result.message}};
}

eos::runtime::dispatch::DispatchResult BrokerCall(const std::string& primitive,
    const std::string& version, const std::string& device_id, json arguments,
    skills::Cancelled cancelled) {
  using Result = eos::runtime::dispatch::DispatchResult;
  if (cancelled && cancelled())
    return {Result::Status::kUnavailable, {}, "skill execution cancelled"};
  const auto found = grants.find({primitive, version, device_id});
  if (found == grants.end())
    return {Result::Status::kFailedPrecondition, {}, "no resolved hardware capability grant"};
  const json body = BrokerRequest({{"kind", "primitive"},
                                  {"token", found->second.token}, {"primitive", primitive},
                                  {"version", version}, {"device_id", found->second.device_id},
                                  {"arguments", std::move(arguments)}});
  return {static_cast<Result::Status>(body.at("status").get<int>()),
          body.at("payload"), body.value("message", "")};
}

json BrokerReadResource(const std::string& uri) {
  const auto reply = BrokerRequest({{"kind", "resource.read"}, {"uri", uri}});
  if (!reply.value("ok", false))
    throw std::runtime_error(reply.value("message", "resource capability denied"));
  return reply.at("value");
}

skills::InferenceResult BrokerInfer(const skills::InferenceRequest& request,
                                    skills::Cancelled cancelled) {
  if (cancelled && cancelled()) throw std::runtime_error("inference cancelled");
  const auto reply = BrokerRequest({{"kind", "inference.call"},
      {"authority", static_cast<int>(request.authority)}, {"source", request.source},
      {"source_sequence", request.source_sequence},
      {"source_generation", request.source_generation}, {"options", request.options}});
  if (!reply.value("ok", false))
    throw std::runtime_error(reply.value("message", "inference capability denied"));
  const auto& value = reply.at("value");
  return {static_cast<skills::InferenceAuthority>(value.at("authority").get<int>()),
          value.at("source").get<std::string>(),
          value.at("source_sequence").get<std::uint64_t>(),
          value.at("source_generation").get<std::uint64_t>(), value.at("value")};
}

std::optional<skills::SourceSample> BrokerSource(json request) {
  const auto reply = BrokerRequest(std::move(request));
  if (!reply.value("ok", false))
    throw std::runtime_error(reply.value("message", "source capability denied"));
  if (reply.at("value").is_null()) return std::nullopt;
  const auto& value = reply.at("value");
  skills::SourceSample sample;
  sample.topic = value.at("topic").get<std::string>();
  sample.source_timestamp_ns = value.at("source_timestamp_ns").get<std::int64_t>();
  sample.sequence = value.at("sequence").get<std::uint64_t>();
  sample.value = value.at("value");
  return sample;
}

eos::runtime::dispatch::DispatchResult BrokerCancel(const std::string& command_id) {
  using Result = eos::runtime::dispatch::DispatchResult;
  // Cancellation is a one-way control packet. The active broker call owns the
  // reply stream; its terminal response establishes whether hardware stopped.
  sw::Send(kControlFd, {sw::MessageType::kCancel, 0, current_generation,
                       json{{"execution_id", current_execution_id},
                            {"command_id", command_id}}.dump()});
  return {Result::Status::kCompleted, {{"command_id", command_id},
                                       {"status", "cancel_requested"}}, {}};
}

int Run() {
  const auto peer = sw::Credentials(kControlFd);
  if (peer.uid != getuid() || peer.pid != getppid())
    throw std::runtime_error("worker peer identity mismatch");
  auto hello = sw::Receive(kControlFd);
  if (!hello || hello->type != sw::MessageType::kHello)
    throw std::runtime_error("worker expected hello");
  const json config = json::parse(hello->payload);
  const auto skill_directory = config.at("directory").get<std::string>();
  sw::ApplySandbox(skill_directory);
  std::vector<skills::PrimitiveReference> primitives;
  for (const auto& p : config.at("primitives"))
    primitives.push_back({p.at("name").get<std::string>(), p.at("version").get<std::string>()});
  ns::Runtime runtime = ns::Runtime::LoadDirectoryWithBroker(
      skill_directory, primitives, BrokerCall, BrokerCancel,
      BrokerReadResource, {}, BrokerInfer);
  json catalog = json::array();
  for (const auto& metadata : runtime.Discover()) catalog.push_back(ToJson(metadata));
  sw::Send(kControlFd, {sw::MessageType::kCatalog, hello->request_id, 0, catalog.dump()});
  for (;;) {
    auto message = sw::Receive(kControlFd);
    if (!message || message->type == sw::MessageType::kStop) return 0;
    if (message->type != sw::MessageType::kExecute)
      throw std::runtime_error("worker expected execute or stop");
    const json request = json::parse(message->payload);
    grants.clear();
    current_generation = message->generation;
    current_execution_id = request.value("execution_id", "");
    for (const auto& grant : request.value("grants", json::array())) {
      grants.emplace(std::make_tuple(grant.at("primitive").get<std::string>(),
                                     grant.at("version").get<std::string>(),
                                     grant.at("selector").get<std::string>()),
                     WorkerGrant{grant.at("token").get<std::string>(),
                                 grant.at("device_id").get<std::string>()});
    }
    skills::ExecutionHooks hooks;
    hooks.execution_id = current_execution_id;
    hooks.runtime_id = request.value("runtime_id", "");
    if (!current_execution_id.empty()) {
    hooks.emit_checkpoint = [](const skills::Checkpoint& checkpoint) {
      return BrokerRequest({{"kind", "checkpoint.emit"}, {"name", checkpoint.name},
                            {"message", checkpoint.message},
                            {"progress", checkpoint.progress ? json(*checkpoint.progress) : json(nullptr)},
                            {"fields", checkpoint.fields}}).value("ok", false);
    };
    hooks.receive_signal = [](std::chrono::milliseconds timeout) -> std::optional<skills::Signal> {
      const auto reply = BrokerRequest({{"kind", "signal.receive"},
                                        {"timeout_ms", timeout.count()}});
      if (!reply.value("ok", false))
        throw std::runtime_error(reply.value("message", "signal receive denied"));
      if (reply.at("value").is_null()) return std::nullopt;
      const auto& value = reply.at("value");
      return skills::Signal{value.at("name").get<std::string>(), value.at("payload"),
                            value.value("signal_id", ""), value.value("execution_id", "")};
    };
    hooks.emit_signal = [](const skills::Signal& signal) {
      return BrokerRequest({{"kind", "signal.emit"}, {"name", signal.name},
                            {"payload", signal.payload},
                            {"signal_id", signal.signal_id}}).value("ok", false);
    };
    hooks.send_community_message = [](const std::string& recipient,
                                      const std::string& channel, json payload,
                                      const std::string& request_id,
                                      std::chrono::milliseconds ttl) {
      if (payload.dump().size() > skills::kMaxCommunityPayloadBytes)
        throw std::length_error("community payload exceeds 16 KiB");
      const auto reply = BrokerRequest({{"kind", "community.send"},
          {"recipient_execution_id", recipient}, {"channel", channel},
          {"payload", std::move(payload)}, {"request_id", request_id},
          {"timeout_ms", ttl.count()}});
      if (!reply.value("ok", false))
        throw std::runtime_error(reply.value("message", "community send denied"));
      return reply.value("sent", false);
    };
    hooks.publish_topic = [](const std::string& topic, json payload,
                             const std::string& request_id, std::chrono::milliseconds ttl) {
      if (payload.dump().size() > skills::kMaxCommunityPayloadBytes)
        throw std::length_error("topic payload exceeds 16 KiB");
      const auto reply = BrokerRequest({{"kind", "community.publish"}, {"channel", topic},
          {"payload", std::move(payload)}, {"request_id", request_id},
          {"timeout_ms", ttl.count()}});
      if (!reply.value("ok", false))
        throw std::runtime_error(reply.value("message", "topic publication denied"));
      return skills::TopicPublishResult{reply.at("sequence").get<std::uint64_t>(),
          reply.at("delivered").get<std::size_t>(), reply.at("dropped").get<std::size_t>()};
    };
    hooks.receive_message = [](const std::string& channel,
                               std::chrono::milliseconds timeout) -> std::optional<skills::IpcMessage> {
      const auto reply = BrokerRequest({{"kind", "community.receive"},
          {"channel", channel}, {"timeout_ms", timeout.count()}});
      if (!reply.value("ok", false))
        throw std::runtime_error(reply.value("message", "community receive denied"));
      if (reply.at("value").is_null()) return std::nullopt;
      const auto& value = reply.at("value");
      skills::IpcMessage message;
      message.message_id = value.at("message_id").get<std::string>();
      message.request_id = value.at("request_id").get<std::string>();
      message.correlation_id = value.at("correlation_id").get<std::string>();
      message.sender_execution_id = value.at("sender_execution_id").get<std::string>();
      message.channel = value.at("channel").get<std::string>();
      message.topic_message = value.at("topic_message").get<bool>();
      message.event_time.sequence = value.at("sequence").get<std::uint64_t>();
      message.dropped_before = value.at("dropped_before").get<std::uint64_t>();
      message.payload = value.at("payload");
      return message;
    };
    hooks.publish_source = [](const std::string& topic, std::int64_t timestamp, json value) {
      auto sample = BrokerSource({{"kind", "source.publish"}, {"topic", topic},
          {"source_timestamp_ns", timestamp}, {"value", std::move(value)}});
      if (!sample) throw std::runtime_error("source publish returned no sample");
      return *sample;
    };
    hooks.query_source = [](const std::string& topic, skills::SourceLookup lookup,
                            std::int64_t timestamp) {
      return BrokerSource({{"kind", "source.query"}, {"topic", topic},
          {"lookup", static_cast<int>(lookup)}, {"source_timestamp_ns", timestamp}});
    };
    hooks.wait_source = [](const std::string& topic, std::uint64_t after,
                           std::chrono::milliseconds timeout) {
      return BrokerSource({{"kind", "source.wait"}, {"topic", topic},
          {"after_sequence", after}, {"timeout_ms", timeout.count()}});
    };
    }
    const auto result = runtime.ExecuteWithHooks(request.at("name").get<std::string>(),
                                                 request.at("version").get<std::string>(),
                                                 request.at("arguments"),
                                                 request.at("command_id").get<std::string>(),
                                                 std::move(hooks));
    sw::Send(kControlFd, {sw::MessageType::kResult, message->request_id,
                         message->generation, ToJson(result).dump()});
    grants.clear();
  }
}
}  // namespace

int main() {
  try { return Run(); }
  catch (const std::exception& e) { std::cerr << "eos-skill-worker: " << e.what() << '\n'; return 1; }
}
