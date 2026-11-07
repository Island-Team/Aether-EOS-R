#include "adapters/eos-cxx/runtime/skills/worker/provider.h"

#include <nlohmann/json-schema.hpp>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <set>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <memory>
#include <stdexcept>
#include <utility>

#include "adapters/eos-cxx/runtime/skills/worker/protocol.h"
#include "adapters/eos-cxx/runtime/skills/core/admission.h"
#include "adapters/eos-cxx/runtime/kernel/broker.h"

namespace eos::runtime::skills::worker {
namespace {
using json = nlohmann::json;
class Session {
 public:
  explicit Session(const std::filesystem::path& executable) {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0)
      throw std::runtime_error("cannot create skill worker socket");
    pid_ = fork();
    if (pid_ < 0) {
      close(sockets[0]); close(sockets[1]);
      throw std::runtime_error("cannot fork skill worker");
    }
    if (pid_ == 0) {
      close(sockets[0]);
      if (dup2(sockets[1], 3) < 0) _exit(125);
      if (sockets[1] != 3) close(sockets[1]);
      if (fcntl(3, F_SETFD, 0) < 0) _exit(125);
      // The worker inherits only stdio and its control channel. In particular,
      // it cannot inherit backend sockets or model credentials from eosd.
      if (syscall(SYS_close_range, 4u, ~0u, 0u) != 0) _exit(125);
      execl(executable.c_str(), executable.c_str(), nullptr);
      _exit(126);
    }
    close(sockets[1]);
    fd_ = sockets[0];
    const auto peer = Credentials(fd_);
    // SO_PEERCRED on a socketpair is captured when the pair is created,
    // before fork; the child end therefore reports the parent PID here.
    if (peer.pid != getpid() || peer.uid != getuid())
      throw std::runtime_error("skill worker peer identity mismatch");
  }
  ~Session() {
    if (fd_ >= 0) close(fd_);
    if (pid_ > 0) {
      kill(pid_, SIGTERM);
      int status = 0;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
      while (std::chrono::steady_clock::now() < deadline) {
        const pid_t reaped = waitpid(pid_, &status, WNOHANG);
        if (reaped == pid_ || (reaped < 0 && errno == ECHILD)) return;
        if (reaped < 0 && errno != EINTR) break;
        usleep(10000);
      }
      kill(pid_, SIGKILL);
      while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    }
  }
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  int fd() const { return fd_; }
  bool Ready(int timeout_ms) const {
    pollfd pfd{fd_, POLLIN, 0};
    const int result = poll(&pfd, 1, timeout_ms);
    if (result < 0 && errno != EINTR) throw std::runtime_error("skill worker poll failed");
    return result > 0;
  }
 private:
  int fd_{-1};
  pid_t pid_{-1};
};

Metadata ParseMetadata(const json& m) {
  Metadata out;
  out.name = m.at("name").get<std::string>();
  out.version = m.at("version").get<std::string>();
  out.description = m.at("description").get<std::string>();
  out.library = m.at("library").get<std::string>();
  out.library_version = m.at("library_version").get<std::string>();
  out.build_id = m.at("build_id").get<std::string>();
  out.parameters_schema = m.at("parameters_schema");
  out.returns_schema = m.at("returns_schema");
  if (!m.at("execution_state_schema").is_null())
    out.execution_state_schema = m.at("execution_state_schema");
  if (!m.at("related_process_state_schema").is_null())
    out.related_process_state_schema = m.at("related_process_state_schema");
  for (const auto& p : m.at("primitives"))
    out.primitives.push_back({p.at("name").get<std::string>(), p.at("version").get<std::string>()});
  for (const auto& r : m.at("resources"))
    out.resources.push_back({r.at("uri_template").get<std::string>(), r.at("description").get<std::string>()});
  for (const auto& c : m.at("claims"))
    out.claim_templates.push_back({static_cast<ClaimKind>(c.at("kind").get<int>()),
                                   static_cast<ClaimMode>(c.at("mode").get<int>()),
                                   c.at("argument_pointer").get<std::string>()});
  for (const auto& s : m.at("accepted_signals"))
    out.accepted_signals.push_back({s.at("name").get<std::string>(), s.at("schema")});
  for (const auto& s : m.at("emitted_signals"))
    out.emitted_signals.push_back({s.at("name").get<std::string>(), s.at("schema")});
  for (const auto& s : m.at("sources"))
    out.sources.push_back({s.at("topic").get<std::string>(), s.at("schema"),
                           static_cast<SourceAuthority>(s.at("authority").get<int>())});
  for (const auto& a : m.at("inference_authorities"))
    out.inference_authorities.push_back(static_cast<InferenceAuthority>(a.get<int>()));
  for (const auto& c : m.at("ipc_channels"))
    out.ipc_channels.push_back({c.at("name").get<std::string>(), c.at("schema"),
                                c.at("can_send").get<bool>(), c.at("can_receive").get<bool>()});
  out.supports_signals = m.at("supports_signals").get<bool>();
  out.reentrant = m.at("reentrant").get<bool>();
  return out;
}

RunResult ParseResult(const json& result) {
  RunResult out;
  out.status = static_cast<RunResult::Status>(result.at("status").get<int>());
  out.accepted = result.at("accepted").get<bool>();
  out.reason_code = result.at("reason_code").get<std::string>();
  out.result = result.at("result");
  out.primitive_acks = result.at("primitive_acks").get<std::vector<json>>();
  out.message = result.at("message").get<std::string>();
  return out;
}
json PrimitiveCatalog(const std::vector<PrimitiveReference>& primitives) {
  json out = json::array();
  for (const auto& p : primitives) out.push_back({{"name", p.name}, {"version", p.version}});
  return out;
}
void Hello(Session& session, const std::filesystem::path& directory,
           const std::vector<PrimitiveReference>& primitives,
           std::vector<Metadata>* catalog) {
  Send(session.fd(), {MessageType::kHello, 1, 0,
                      json{{"directory", directory.string()},
                           {"primitives", PrimitiveCatalog(primitives)}}.dump()});
  if (!session.Ready(10000)) throw std::runtime_error("skill worker catalog timed out");
  auto reply = Receive(session.fd());
  if (!reply || reply->type != MessageType::kCatalog || reply->request_id != 1)
    throw std::runtime_error("skill worker catalog unavailable");
  if (catalog)
    for (const auto& m : json::parse(reply->payload)) catalog->push_back(ParseMetadata(m));
}
}  // namespace

NativeWorkerProvider::NativeWorkerProvider(std::filesystem::path worker_executable,
    std::filesystem::path skill_directory, dispatch::Dispatcher& dispatcher,
    GrantResolver resolve_grants, ResourceReader read_resource, InferenceRunner infer)
    : worker_executable_(std::move(worker_executable)),
      skill_directory_(std::move(skill_directory)), dispatcher_(dispatcher),
      resolve_grants_(std::move(resolve_grants)),
      read_resource_(std::move(read_resource)), infer_(std::move(infer)) {
  if (!worker_executable_.is_absolute() || !skill_directory_.is_absolute())
    throw std::invalid_argument("worker executable and skill directory must be absolute");
  for (const auto& p : dispatcher_.PrimitiveIdentities())
    primitives_.push_back({p.name, p.version});
  Session session(worker_executable_);
  Hello(session, skill_directory_, primitives_, &catalog_);
  for (std::size_t i = 0; i < catalog_.size(); ++i)
    if (!by_identity_.emplace(std::make_pair(catalog_[i].name, catalog_[i].version), i).second)
      throw std::runtime_error("duplicate worker skill identity");
  constexpr std::size_t kConcurrentWorkers = 8;
  for (std::size_t i = 0; i < kConcurrentWorkers; ++i) {
    jobs_workers_.emplace_back([this] {
      for (;;) {
        std::function<void()> job;
        {
          std::unique_lock lock(jobs_mutex_);
          jobs_ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
          if (stopping_ && jobs_.empty()) return;
          job = std::move(jobs_.front());
          jobs_.pop_front();
        }
        job();
      }
    });
  }
}

NativeWorkerProvider::NativeWorkerProvider(std::filesystem::path worker_executable,
    std::filesystem::path skill_directory, dispatch::Dispatcher& dispatcher,
    kernel::Broker& broker, const kernel::Catalog& catalog, GrantResolver resolve_grants,
    ResourceReader read_resource, InferenceRunner infer)
    : NativeWorkerProvider(std::move(worker_executable), std::move(skill_directory), dispatcher,
                           std::move(resolve_grants), std::move(read_resource),
                           std::move(infer)) {
  kernel_broker_ = &broker;
  kernel_catalog_ = &catalog;
}

NativeWorkerProvider::~NativeWorkerProvider() {
  {
    std::lock_guard lock(jobs_mutex_);
    stopping_ = true;
  }
  jobs_ready_.notify_all();
  for (auto& worker : jobs_workers_) worker.join();
}

void NativeWorkerProvider::ExecuteAsync(const std::string& name, const std::string& version,
    const json& arguments, const std::string& command_id, ExecutionHooks hooks,
    RunCompletion completion) const {
  std::function<void()> job = [this, name, version, arguments, command_id,
                               hooks = std::move(hooks), completion = std::move(completion)]() mutable {
    RunResult result;
    try {
      result = ExecuteWithHooks(name, version, arguments, command_id, std::move(hooks));
    } catch (const std::exception& error) {
      result = {RunResult::Status::kFailed, false, "worker.execution", {}, {}, error.what()};
    }
    completion(std::move(result));
  };
  {
    std::lock_guard lock(jobs_mutex_);
    if (stopping_ || jobs_.size() >= kMaxQueuedExecutions)
      throw std::runtime_error("skill worker execution capacity exhausted");
    jobs_.push_back(std::move(job));
  }
  jobs_ready_.notify_one();
}

std::vector<Metadata> NativeWorkerProvider::Discover() const { return catalog_; }
const Metadata* NativeWorkerProvider::Find(const std::string& name, const std::string& version) const {
  const auto it = by_identity_.find({name, version});
  return it == by_identity_.end() ? nullptr : &catalog_[it->second];
}
void NativeWorkerProvider::ValidateArguments(const std::string& name, const std::string& version,
                                              const json& arguments) const {
  const Metadata* metadata = Find(name, version);
  if (!metadata) throw std::invalid_argument("unknown worker skill");
  nlohmann::json_schema::json_validator validator;
  validator.set_root_schema(metadata->parameters_schema);
  validator.validate(arguments);
}
RunResult NativeWorkerProvider::Execute(const std::string& name, const std::string& version,
    const json& arguments, const std::string& command_id, Cancelled cancelled) const {
  ExecutionHooks hooks;
  hooks.execution_id = command_id;
  hooks.cancelled = std::move(cancelled);
  return ExecuteWithHooks(name, version, arguments, command_id, std::move(hooks));
}
RunResult NativeWorkerProvider::ExecuteWithHooks(const std::string& name,
    const std::string& version, const json& arguments, const std::string& command_id,
    ExecutionHooks hooks) const {
  ValidateArguments(name, version, arguments);
  const Metadata& metadata = *Find(name, version);
  if (!metadata.resources.empty() && !read_resource_)
    throw std::invalid_argument("worker resource broker is unavailable for declared resources");
  if (!metadata.inference_authorities.empty() && !infer_)
    throw std::invalid_argument("worker inference broker is unavailable for declared authority");
  if (!metadata.accepted_signals.empty() && !hooks.receive_signal)
    throw std::invalid_argument("worker signal receive broker is unavailable");
  if (!metadata.emitted_signals.empty() && !hooks.emit_signal)
    throw std::invalid_argument("worker signal emit broker is unavailable");
  for (const auto& source : metadata.sources) {
    if (source.authority == SourceAuthority::kRead &&
        (!hooks.query_source || !hooks.wait_source))
      throw std::invalid_argument("worker source read broker is unavailable");
    if (source.authority == SourceAuthority::kWrite && !hooks.publish_source)
      throw std::invalid_argument("worker source publish broker is unavailable");
  }
  Session session(worker_executable_);
  Hello(session, skill_directory_, primitives_, nullptr);
  std::unique_ptr<GrantTable> legacy_grants;
  if (!kernel_broker_) legacy_grants = std::make_unique<GrantTable>();
  struct GrantScope {
    kernel::Broker* broker;
    GrantTable* legacy;
    std::string execution_id;
    bool revoked{false};
    void Revoke() {
      if (revoked) return;
      if (broker) broker->RevokeExecution(execution_id);
      if (legacy) legacy->RevokeExecution(execution_id);
      revoked = true;
    }
    ~GrantScope() { Revoke(); }
  } grant_scope{kernel_broker_, legacy_grants.get(), hooks.execution_id};
  json issued = json::array();
  const std::vector<Grant> resolved = resolve_grants_
      ? resolve_grants_(metadata, arguments, hooks.execution_id) : std::vector<Grant>{};
  const std::uint64_t generation = resolved.empty() ? 0 : resolved.front().generation;
  std::set<std::tuple<std::string, std::string, std::string>> selectors;
  std::map<std::string, Grant> issued_grants;
  for (Grant grant : resolved) {
    if (grant.generation != generation)
      throw std::invalid_argument("worker grants span multiple binding generations");
    const bool declared = std::any_of(metadata.primitives.begin(), metadata.primitives.end(),
        [&](const auto& p) { return p.name == grant.primitive && p.version == grant.version; });
    if (!declared) throw std::invalid_argument("worker grant names undeclared primitive");
    if (grant.selector.empty())
      throw std::invalid_argument("worker grant has no stable selector");
    if (!selectors.emplace(grant.primitive, grant.version, grant.selector).second)
      throw std::invalid_argument("ambiguous worker capability selector");
    grant.execution_id = hooks.execution_id;
    std::string token;
    if (kernel_broker_) {
      const auto* capability = kernel_catalog_->FindHardware(
          grant.primitive, grant.version, grant.device_id);
      if (!capability)
        throw std::invalid_argument("resolved worker primitive has no kernel capability");
      grant.capability = capability->name;
      token = kernel_broker_->Grant(grant.capability, hooks.execution_id,
                                    generation, grant.expires_at);
    } else {
      token = legacy_grants->Issue(grant);
    }
    issued_grants.emplace(token, grant);
    issued.push_back({{"token", token}, {"primitive", grant.primitive},
                      {"version", grant.version}, {"selector", grant.selector},
                      {"device_id", grant.device_id}});
  }
  Send(session.fd(), {MessageType::kExecute, 2, generation,
                      json{{"name", name}, {"version", version},
                           {"arguments", arguments}, {"command_id", command_id},
                           {"execution_id", hooks.execution_id},
                           {"runtime_id", hooks.runtime_id}, {"grants", issued}}.dump()});
  const auto execution_deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
  const auto cancel_command = [this](const std::string& id) {
    return kernel_broker_ ? kernel_broker_->Cancel(id) : dispatcher_.CancelCommand(id);
  };
  const auto get_command = [this](const std::string& id) {
    return kernel_broker_ ? kernel_broker_->Get(id) : dispatcher_.GetCommand(id);
  };
  std::set<std::string> active_commands;
  const auto cancel_active = [&] {
    bool unknown = false;
    for (const auto& id : active_commands) {
      const auto outcome = cancel_command(id);
      if (outcome.status != dispatch::DispatchResult::Status::kCompleted ||
          (outcome.payload.is_object() && outcome.payload.contains("result") &&
           outcome.payload.at("result").is_object() &&
           outcome.payload.at("result").value("status", "") == "outcome_unknown"))
        unknown = true;
    }
    active_commands.clear();
    return unknown;
  };
  for (;;) {
    if (std::chrono::steady_clock::now() >= execution_deadline) {
      grant_scope.Revoke();
      const bool unknown = cancel_active();
      return {RunResult::Status::kFailed, false, unknown ? "worker.outcome_unknown" : "worker.deadline", {}, {},
              "skill worker exceeded its execution deadline"};
    }
    if (hooks.cancelled && hooks.cancelled()) {
      grant_scope.Revoke();
      const bool unknown = cancel_active();
      return {RunResult::Status::kCancelled, false, unknown ? "worker.outcome_unknown" : "cancelled", {}, {},
              "skill worker cancelled"};
    }
    if (!session.Ready(50)) continue;
    auto message = Receive(session.fd());
    if (!message) {
      const bool unknown = cancel_active();
      return {RunResult::Status::kFailed, false,
              unknown ? "worker.outcome_unknown" : "worker.exited", {}, {},
              "skill worker exited before result"};
    }
    if (message->type == MessageType::kResult && message->request_id == 2) {
      const bool unknown = cancel_active();
      if (unknown) return {RunResult::Status::kFailed, false, "worker.outcome_unknown", {}, {},
                           "worker finished with unresolved primitive command"};
      return ParseResult(json::parse(message->payload));
    }
    if (message->type == MessageType::kCancel) {
      const json request = json::parse(message->payload);
      if (message->generation == generation &&
          request.value("execution_id", "") == hooks.execution_id &&
          active_commands.count(request.value("command_id", "")))
        (void)cancel_command(request.at("command_id").get<std::string>());
      continue;
    }
    if (message->type != MessageType::kCapabilityCall)
      return {RunResult::Status::kProtocolError, false, "worker.protocol", {}, {},
              "unexpected worker message"};
    const json call = json::parse(message->payload);
    json reply;
    const std::string kind = call.value("kind", "primitive");
    if (call.value("execution_id", "") != hooks.execution_id ||
        message->generation != generation) {
      if (kind == "primitive")
        reply = {{"status", static_cast<int>(dispatch::DispatchResult::Status::kFailedPrecondition)},
                 {"payload", nullptr}, {"message", "worker execution identity mismatch"}};
      else
        reply = {{"ok", false}, {"message", "worker execution identity mismatch"}};
    } else if (kind == "primitive") {
      dispatch::DispatchResult outcome{dispatch::DispatchResult::Status::kFailedPrecondition,
                                       {}, "hardware capability denied"};
      CapabilityCall requested{call.at("token").get<std::string>(),
                               call.at("execution_id").get<std::string>(),
                               message->generation,
                               call.at("primitive").get<std::string>(),
                               call.at("version").get<std::string>(),
                               call.at("device_id").get<std::string>()};
      const auto grant_it = issued_grants.find(requested.token);
      const bool exact_grant = grant_it != issued_grants.end() &&
          grant_it->second.execution_id == requested.execution_id &&
          grant_it->second.generation == requested.generation &&
          grant_it->second.primitive == requested.primitive &&
          grant_it->second.version == requested.version &&
          grant_it->second.device_id == requested.device_id;
      if (exact_grant && (kernel_broker_ || legacy_grants->Authorize(requested))) {
        const json& step_arguments = call.at("arguments");
        const std::string command_id = step_arguments.value("command_id", "");
        outcome = kernel_broker_
            ? kernel_broker_->Start(grant_it->second.capability, requested.token,
                                    hooks.execution_id, generation, step_arguments, command_id)
            : dispatcher_.StartCommand(requested.primitive, requested.version,
                                       requested.device_id, step_arguments, command_id);
        if (outcome.status == dispatch::DispatchResult::Status::kPending) {
          bool worker_exited = false;
          active_commands.insert(command_id);
          while (outcome.status == dispatch::DispatchResult::Status::kPending) {
            if ((hooks.cancelled && hooks.cancelled()) ||
                std::chrono::steady_clock::now() >= execution_deadline) {
              outcome = cancel_command(command_id);
              break;
            }
            if (session.Ready(10)) {
              auto control = Receive(session.fd());
              if (!control) {
                outcome = cancel_command(command_id);
                worker_exited = true;
                break;
              }
              if (control->type == MessageType::kCancel &&
                  control->generation == generation) {
                const json cancellation = json::parse(control->payload);
                if (cancellation.value("execution_id", "") == hooks.execution_id &&
                    cancellation.value("command_id", "") == command_id)
                  outcome = cancel_command(command_id);
              } else {
                outcome = cancel_command(command_id);
                break;
              }
            }
            if (outcome.status == dispatch::DispatchResult::Status::kPending)
              outcome = get_command(command_id);
          }
          active_commands.erase(command_id);
          if (worker_exited)
            return {RunResult::Status::kFailed, false, "worker.exited", {}, {},
                    "skill worker exited during a primitive command"};
        }
      }
      reply = {{"status", static_cast<int>(outcome.status)},
               {"payload", outcome.payload}, {"message", outcome.message}};
    } else if (kind == "resource.read") {
      const std::string uri = call.at("uri").get<std::string>();
      const bool declared = std::any_of(metadata.resources.begin(), metadata.resources.end(),
          [&](const Resource& r) { return ResourceTemplateMatches(r.uri_template, uri); });
      if (!declared || !read_resource_)
        reply = {{"ok", false}, {"message", "resource capability denied"}};
      else {
        try { reply = {{"ok", true}, {"value", read_resource_(uri)}}; }
        catch (const std::exception& error) {
          reply = {{"ok", false}, {"message", error.what()}};
        }
      }
    } else if (kind == "inference.call") {
      const auto authority = static_cast<InferenceAuthority>(call.at("authority").get<int>());
      const bool declared = std::find(metadata.inference_authorities.begin(),
                                      metadata.inference_authorities.end(), authority) !=
                            metadata.inference_authorities.end();
      if (!declared || !infer_)
        reply = {{"ok", false}, {"message", "inference capability denied"}};
      else {
        try {
          const InferenceRequest request{authority, call.at("source").get<std::string>(),
              call.at("source_sequence").get<std::uint64_t>(),
              call.at("source_generation").get<std::uint64_t>(), call.at("options")};
          const auto result = infer_(request, hooks.cancelled);
          reply = {{"ok", true}, {"value", {{"authority", static_cast<int>(result.authority)},
              {"source", result.source}, {"source_sequence", result.source_sequence},
              {"source_generation", result.source_generation}, {"value", result.value}}}};
        } catch (const std::exception& error) {
          reply = {{"ok", false}, {"message", error.what()}};
        }
      }
    } else if (kind == "checkpoint.emit") {
      const Checkpoint checkpoint{call.at("name").get<std::string>(),
                                  call.at("message").get<std::string>(),
                                  call.at("progress").is_null()
                                      ? std::nullopt
                                      : std::optional<double>(call.at("progress").get<double>()),
                                  call.at("fields")};
      reply = {{"ok", hooks.emit_checkpoint && hooks.emit_checkpoint(checkpoint)}};
    } else if (kind == "signal.receive") {
      const auto timeout = std::chrono::milliseconds(call.at("timeout_ms").get<int>());
      const auto signal = hooks.receive_signal ? hooks.receive_signal(timeout) : std::nullopt;
      if (!signal)
        reply = {{"ok", true}, {"value", nullptr}};
      else {
        const bool declared = signal->name == "CANCEL" ||
            std::any_of(metadata.accepted_signals.begin(), metadata.accepted_signals.end(),
                [&](const SignalDescriptor& s) { return s.name == signal->name; });
        reply = declared
            ? json{{"ok", true}, {"value", {{"name", signal->name},
                  {"payload", signal->payload}, {"signal_id", signal->signal_id},
                  {"execution_id", signal->execution_id}}}}
            : json{{"ok", false}, {"message", "undeclared signal"}};
      }
    } else if (kind == "signal.emit") {
      const Signal signal{call.at("name").get<std::string>(), call.at("payload"),
                          call.value("signal_id", ""), hooks.execution_id};
      const bool declared = std::any_of(metadata.emitted_signals.begin(),
                                       metadata.emitted_signals.end(),
          [&](const SignalDescriptor& s) { return s.name == signal.name; });
      reply = {{"ok", declared && hooks.emit_signal && hooks.emit_signal(signal)}};
    } else if (kind == "community.send" || kind == "community.publish" ||
               kind == "community.receive") {
      const std::string channel = call.at("channel").get<std::string>();
      const bool sending = kind != "community.receive";
      const bool declared = std::any_of(metadata.ipc_channels.begin(), metadata.ipc_channels.end(),
          [&](const IpcChannelDescriptor& item) {
            return item.name == channel && (sending ? item.can_send : item.can_receive);
          });
      if (!declared) reply = {{"ok", false}, {"message", "community channel denied"}};
      else {
        try {
          if (kind == "community.send") {
            if (!hooks.send_community_message) throw std::runtime_error("community unavailable");
            reply = {{"ok", true}, {"sent", hooks.send_community_message(
                call.at("recipient_execution_id").get<std::string>(), channel,
                call.at("payload"), call.at("request_id").get<std::string>(),
                std::chrono::milliseconds(call.at("timeout_ms").get<int>()))}};
          } else if (kind == "community.publish") {
            if (!hooks.publish_topic) throw std::runtime_error("community unavailable");
            const auto result = hooks.publish_topic(channel, call.at("payload"),
                call.at("request_id").get<std::string>(),
                std::chrono::milliseconds(call.at("timeout_ms").get<int>()));
            reply = {{"ok", true}, {"sequence", result.sequence},
                     {"delivered", result.delivered}, {"dropped", result.dropped}};
          } else {
            if (!hooks.receive_message) throw std::runtime_error("community unavailable");
            const auto received = hooks.receive_message(channel,
                std::chrono::milliseconds(call.at("timeout_ms").get<int>()));
            reply = {{"ok", true}, {"value", received
                ? json{{"message_id", received->message_id}, {"request_id", received->request_id},
                       {"correlation_id", received->correlation_id},
                       {"sender_execution_id", received->sender_execution_id},
                       {"channel", received->channel}, {"topic_message", received->topic_message},
                       {"sequence", received->event_time.sequence},
                       {"dropped_before", received->dropped_before}, {"payload", received->payload}}
                : json(nullptr)}};
          }
        } catch (const std::exception& error) {
          reply = {{"ok", false}, {"message", error.what()}};
        }
      }
    } else if (kind == "source.publish" || kind == "source.query" ||
               kind == "source.wait") {
      const std::string topic = call.at("topic").get<std::string>();
      const SourceAuthority authority = kind == "source.publish"
          ? SourceAuthority::kWrite : SourceAuthority::kRead;
      const bool declared = std::any_of(metadata.sources.begin(), metadata.sources.end(),
          [&](const SourceDescriptor& source) {
            return source.topic == topic && source.authority == authority;
          });
      if (!declared) {
        reply = {{"ok", false}, {"message", "source capability denied"}};
      } else {
        try {
          std::optional<SourceSample> sample;
          if (kind == "source.publish" && hooks.publish_source)
            sample = hooks.publish_source(topic, call.at("source_timestamp_ns").get<std::int64_t>(),
                                          call.at("value"));
          else if (kind == "source.query" && hooks.query_source)
            sample = hooks.query_source(topic,
                static_cast<SourceLookup>(call.at("lookup").get<int>()),
                call.at("source_timestamp_ns").get<std::int64_t>());
          else if (kind == "source.wait" && hooks.wait_source)
            sample = hooks.wait_source(topic, call.at("after_sequence").get<std::uint64_t>(),
                std::chrono::milliseconds(call.at("timeout_ms").get<int>()));
          else
            throw std::runtime_error("source capability unavailable");
          reply = {{"ok", true}, {"value", sample
              ? json{{"topic", sample->topic},
                     {"source_timestamp_ns", sample->source_timestamp_ns},
                     {"sequence", sample->sequence}, {"value", sample->value}}
              : json(nullptr)}};
        } catch (const std::exception& error) {
          reply = {{"ok", false}, {"message", error.what()}};
        }
      }
    } else {
      reply = {{"ok", false}, {"message", "unknown worker capability"}};
    }
    Send(session.fd(), {MessageType::kCapabilityReply, message->request_id,
                        message->generation, reply.dump()});
  }
}
}  // namespace eos::runtime::skills::worker
