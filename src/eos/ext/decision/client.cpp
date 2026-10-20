#include "client.h"

#include <google/protobuf/util/json_util.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <mutex>
#include <utility>

#include "eos/skills.grpc.pb.h"

namespace eos::ext::decision {
namespace api = island::eos::v1;
namespace {

Json StructJson(const google::protobuf::Struct& value) {
  std::string text;
  if (!google::protobuf::util::MessageToJsonString(value, &text).ok()) return Json::object();
  return Json::parse(text, nullptr, false);
}

std::chrono::system_clock::time_point Time(const google::protobuf::Timestamp& value) {
  return std::chrono::system_clock::time_point(std::chrono::duration_cast<
                                               std::chrono::system_clock::duration>(
      std::chrono::seconds(value.seconds()) + std::chrono::nanoseconds(value.nanos())));
}

std::chrono::milliseconds Duration(const google::protobuf::Duration& value) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::seconds(value.seconds()) + std::chrono::nanoseconds(value.nanos()));
}

bool JsonStruct(const Json& value, google::protobuf::Struct* output) {
  return value.is_object() &&
         google::protobuf::util::JsonStringToMessage(value.dump(), output).ok();
}

SkillReference Reference(const api::SkillReference& value) {
  return {value.name(), value.version()};
}

void Reference(const SkillReference& value, api::SkillReference* output) {
  output->set_name(value.name);
  output->set_version(value.version);
}

ClaimKind Kind(api::SkillClaimKind kind) {
  switch (kind) {
    case api::SKILL_CLAIM_KIND_DEVICE:
      return ClaimKind::kDevice;
    case api::SKILL_CLAIM_KIND_RESOURCE:
      return ClaimKind::kResource;
    case api::SKILL_CLAIM_KIND_PROVIDER_CONTEXT:
      return ClaimKind::kProviderContext;
    default:
      return ClaimKind::kUnspecified;
  }
}

ClaimMode Mode(api::SkillClaimMode mode) {
  switch (mode) {
    case api::SKILL_CLAIM_MODE_SHARED_READ:
      return ClaimMode::kSharedRead;
    case api::SKILL_CLAIM_MODE_EXCLUSIVE_USE:
      return ClaimMode::kExclusiveUse;
    default:
      return ClaimMode::kUnspecified;
  }
}

SignalDescriptor Descriptor(const api::SkillSignalDescriptor& value) {
  return {value.name(), StructJson(value.payload_schema())};
}

SkillDescriptor Descriptor(const api::SkillDescriptor& value) {
  SkillDescriptor result;
  result.reference = Reference(value.reference());
  result.description = value.description();
  result.parameters = StructJson(value.parameters());
  result.returns = StructJson(value.returns());
  result.reentrant = value.reentrant();
  for (const auto& primitive : value.primitives())
    result.primitives.push_back({primitive.name(), primitive.version()});
  for (const auto& resource : value.resources())
    result.resources.push_back({resource.uri_template(), resource.description()});
  for (const auto& signal : value.accepted_signals())
    result.accepted_signals.push_back(Descriptor(signal));
  for (const auto& signal : value.emitted_signals())
    result.emitted_signals.push_back(Descriptor(signal));
  for (const auto& claim : value.claims()) {
    result.claims.push_back(
        {Kind(claim.kind()), Mode(claim.mode()), claim.argument_json_pointer()});
  }
  return result;
}

ExecutionStatus Status(api::ExecutionStatus status) {
  switch (status) {
    case api::EXECUTION_STATUS_QUEUED:
      return ExecutionStatus::kQueued;
    case api::EXECUTION_STATUS_RUNNING:
      return ExecutionStatus::kRunning;
    case api::EXECUTION_STATUS_SUCCEEDED:
      return ExecutionStatus::kSucceeded;
    case api::EXECUTION_STATUS_REJECTED:
      return ExecutionStatus::kRejected;
    case api::EXECUTION_STATUS_CANCELLED:
      return ExecutionStatus::kCancelled;
    case api::EXECUTION_STATUS_TIMED_OUT:
      return ExecutionStatus::kTimedOut;
    case api::EXECUTION_STATUS_FAILED:
      return ExecutionStatus::kFailed;
    default:
      return ExecutionStatus::kUnspecified;
  }
}

api::ExecutionStatus Status(ExecutionStatus status) {
  switch (status) {
    case ExecutionStatus::kQueued:
      return api::EXECUTION_STATUS_QUEUED;
    case ExecutionStatus::kRunning:
      return api::EXECUTION_STATUS_RUNNING;
    case ExecutionStatus::kSucceeded:
      return api::EXECUTION_STATUS_SUCCEEDED;
    case ExecutionStatus::kRejected:
      return api::EXECUTION_STATUS_REJECTED;
    case ExecutionStatus::kCancelled:
      return api::EXECUTION_STATUS_CANCELLED;
    case ExecutionStatus::kTimedOut:
      return api::EXECUTION_STATUS_TIMED_OUT;
    case ExecutionStatus::kFailed:
      return api::EXECUTION_STATUS_FAILED;
    default:
      return api::EXECUTION_STATUS_UNSPECIFIED;
  }
}

ExecutionView Execution(const api::SkillExecution& value) {
  ExecutionView result;
  result.execution_id = value.execution_id();
  result.runtime_id = value.runtime_id();
  result.request_id = value.request_id();
  result.reference = Reference(value.reference());
  result.status = Status(value.status());
  result.sequence = value.sequence();
  result.latest_journal_sequence = value.latest_journal_sequence();
  result.cancellation_requested = value.cancellation_requested();
  result.queue_reason = value.queue_reason();
  for (const auto& claim : value.claims())
    result.claims.push_back({Kind(claim.kind()), Mode(claim.mode()), claim.identity()});
  result.reason_code = value.reason_code();
  result.message = value.message();
  result.result = StructJson(value.result());
  if (value.has_created_at()) result.created_at = Time(value.created_at());
  if (value.has_deadline_at()) result.deadline_at = Time(value.deadline_at());
  if (value.has_started_at()) result.started_at = Time(value.started_at());
  if (value.has_cancellation_requested_at())
    result.cancellation_requested_at = Time(value.cancellation_requested_at());
  if (value.has_finished_at()) result.finished_at = Time(value.finished_at());
  if (value.has_queue_duration()) result.queue_duration = Duration(value.queue_duration());
  if (value.has_run_duration()) result.run_duration = Duration(value.run_duration());
  if (value.has_total_duration()) result.total_duration = Duration(value.total_duration());
  if (value.has_current_activity()) {
    ExecutionView::CurrentActivity activity;
    activity.activity_id = value.current_activity().activity_id();
    activity.name = value.current_activity().name();
    if (value.current_activity().has_started_at())
      activity.started_at = Time(value.current_activity().started_at());
    activity.fields = StructJson(value.current_activity().fields());
    result.current_activity = std::move(activity);
  }
  if (value.has_accounting()) {
    const auto& accounting = value.accounting();
    result.accounting.primitive_calls = accounting.primitive_calls();
    result.accounting.resource_reads = accounting.resource_reads();
    result.accounting.signals_received = accounting.signals_received();
    result.accounting.signals_emitted = accounting.signals_emitted();
    result.accounting.diagnostic_logs = accounting.diagnostic_logs();
    result.accounting.diagnostic_logs_dropped = accounting.diagnostic_logs_dropped();
    result.accounting.checkpoints = accounting.checkpoints();
    result.accounting.checkpoints_dropped = accounting.checkpoints_dropped();
    result.accounting.activity_events = accounting.activity_events();
    result.accounting.activity_events_dropped = accounting.activity_events_dropped();
    if (accounting.has_claim_wait_duration())
      result.accounting.claim_wait_duration = Duration(accounting.claim_wait_duration());
    if (accounting.has_signal_wait_duration())
      result.accounting.signal_wait_duration = Duration(accounting.signal_wait_duration());
    if (accounting.has_capability_duration())
      result.accounting.capability_duration = Duration(accounting.capability_duration());
  }
  return result;
}

SignalView::Direction Direction(api::SkillSignalDirection direction) {
  switch (direction) {
    case api::SKILL_SIGNAL_DIRECTION_AGENT_TO_SKILL:
      return SignalView::Direction::kAgentToSkill;
    case api::SKILL_SIGNAL_DIRECTION_SKILL_TO_AGENT:
      return SignalView::Direction::kSkillToAgent;
    default:
      return SignalView::Direction::kUnspecified;
  }
}

DecisionJournalEntry JournalEntry(const api::SkillJournalEntry& value) {
  DecisionJournalEntry result;
  result.execution_id = value.execution_id();
  result.journal_sequence = value.journal_sequence();
  if (value.has_timestamp()) result.timestamp = Time(value.timestamp());
  switch (value.entry_case()) {
    case api::SkillJournalEntry::kSnapshot:
      result.value = Execution(value.snapshot());
      break;
    case api::SkillJournalEntry::kSignal: {
      SignalView signal;
      signal.signal_id = value.signal().signal_id();
      signal.execution_id = value.signal().execution_id();
      signal.direction = Direction(value.signal().direction());
      signal.name = value.signal().name();
      signal.payload = StructJson(value.signal().payload());
      result.value = std::move(signal);
      break;
    }
    case api::SkillJournalEntry::kRecord: {
      JournalRecordView record;
      record.event_id = value.record().event_id();
      record.execution_id = value.record().execution_id();
      switch (value.record().source()) {
        case api::SKILL_JOURNAL_SOURCE_SYSTEM:
          record.source = JournalRecordView::Source::kSystem;
          break;
        case api::SKILL_JOURNAL_SOURCE_SKILL:
          record.source = JournalRecordView::Source::kSkill;
          break;
        default:
          break;
      }
      switch (value.record().kind()) {
        case api::SKILL_JOURNAL_KIND_ACTIVITY:
          record.kind = JournalRecordView::Kind::kActivity;
          break;
        case api::SKILL_JOURNAL_KIND_CHECKPOINT:
          record.kind = JournalRecordView::Kind::kCheckpoint;
          break;
        case api::SKILL_JOURNAL_KIND_SUPPRESSION:
          record.kind = JournalRecordView::Kind::kSuppression;
          break;
        default:
          break;
      }
      record.name = value.record().name();
      record.outcome = value.record().outcome();
      record.message = value.record().message();
      record.timestamp = result.timestamp;
      if (value.record().has_elapsed()) record.elapsed = Duration(value.record().elapsed());
      if (value.record().has_duration()) record.duration = Duration(value.record().duration());
      if (value.record().progress_value_case() == api::SkillJournalRecord::kProgress) {
        record.progress = value.record().progress();
      }
      record.fields = StructJson(value.record().fields());
      result.value = std::move(record);
      break;
    }
    case api::SkillJournalEntry::ENTRY_NOT_SET:
      result.value = std::monostate{};
      break;
  }
  return result;
}

MonitorUpdateView MonitorUpdate(const api::SkillMonitorUpdate& value) {
  MonitorUpdateView result;
  result.monitor_sequence = value.monitor_sequence();
  result.reset = value.reset();
  result.upserted.reserve(value.upserted_size());
  for (const auto& execution : value.upserted()) result.upserted.push_back(Execution(execution));
  result.removed_execution_ids.assign(value.removed_execution_ids().begin(),
                                      value.removed_execution_ids().end());
  return result;
}

TransportError Error(const grpc::Status& status) {
  return {static_cast<int>(status.error_code()), status.error_message()};
}

template <typename T>
Result<T> Failure(const grpc::Status& status) {
  return Result<T>::Failure(static_cast<int>(status.error_code()), status.error_message());
}

}  // namespace

struct SkillClient::Impl {
  explicit Impl(std::shared_ptr<grpc::Channel> channel, SkillClientOptions value)
      : stub(api::SkillService::NewStub(std::move(channel))), options(value) {}

  struct ActiveCall {
    Impl& owner;
    grpc::ClientContext* context;
    ActiveCall(Impl& owner_value, grpc::ClientContext* context_value)
        : owner(owner_value), context(context_value) {
      std::lock_guard<std::mutex> lock(owner.mutex);
      owner.active.push_back(context);
    }
    ~ActiveCall() {
      std::lock_guard<std::mutex> lock(owner.mutex);
      owner.active.erase(std::remove(owner.active.begin(), owner.active.end(), context),
                         owner.active.end());
    }
  };

  void Deadline(grpc::ClientContext* context) const {
    context->set_deadline(std::chrono::system_clock::now() + options.unary_timeout);
  }

  std::unique_ptr<api::SkillService::Stub> stub;
  SkillClientOptions options;
  std::mutex mutex;
  std::vector<grpc::ClientContext*> active;
};

SkillClient::SkillClient(std::shared_ptr<grpc::Channel> channel, SkillClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(channel), options)) {}

SkillClient::~SkillClient() { CancelPendingCalls(); }

Result<std::vector<SkillDescriptor>> SkillClient::ListSkills() {
  grpc::ClientContext context;
  Impl::ActiveCall active(*impl_, &context);
  impl_->Deadline(&context);
  api::ListSkillsRequest request;
  api::ListSkillsResponse response;
  const auto status = impl_->stub->ListSkills(&context, request, &response);
  if (!status.ok()) return Failure<std::vector<SkillDescriptor>>(status);
  std::vector<SkillDescriptor> result;
  result.reserve(response.skills_size());
  for (const auto& skill : response.skills()) result.push_back(Descriptor(skill));
  return Result<std::vector<SkillDescriptor>>::Success(std::move(result));
}

Result<SkillDescriptor> SkillClient::GetSkill(const SkillReference& reference) {
  grpc::ClientContext context;
  Impl::ActiveCall active(*impl_, &context);
  impl_->Deadline(&context);
  api::GetSkillRequest request;
  Reference(reference, request.mutable_reference());
  api::SkillDescriptor response;
  const auto status = impl_->stub->GetSkill(&context, request, &response);
  return status.ok() ? Result<SkillDescriptor>::Success(Descriptor(response))
                     : Failure<SkillDescriptor>(status);
}

Result<ExecutionView> SkillClient::StartExecution(const StartAction& action) {
  grpc::ClientContext context;
  Impl::ActiveCall active(*impl_, &context);
  impl_->Deadline(&context);
  api::StartExecutionRequest request;
  Reference(action.reference, request.mutable_reference());
  request.set_request_id(action.request_id);
  if (action.timeout) {
    request.mutable_timeout()->set_seconds(action.timeout->count() / 1000);
    request.mutable_timeout()->set_nanos((action.timeout->count() % 1000) * 1000000);
  }
  if (!JsonStruct(action.arguments, request.mutable_arguments()))
    return Result<ExecutionView>::Failure(3, "arguments must be a JSON object");
  api::SkillExecution response;
  const auto status = impl_->stub->StartExecution(&context, request, &response);
  return status.ok() ? Result<ExecutionView>::Success(Execution(response))
                     : Failure<ExecutionView>(status);
}

Result<std::vector<ExecutionView>> SkillClient::ListExecutions() {
  grpc::ClientContext context;
  Impl::ActiveCall active(*impl_, &context);
  impl_->Deadline(&context);
  api::ListExecutionsRequest request;
  api::ListExecutionsResponse response;
  const auto status = impl_->stub->ListExecutions(&context, request, &response);
  if (!status.ok()) return Failure<std::vector<ExecutionView>>(status);
  std::vector<ExecutionView> result;
  result.reserve(response.executions_size());
  for (const auto& execution : response.executions()) result.push_back(Execution(execution));
  return Result<std::vector<ExecutionView>>::Success(std::move(result));
}

Result<ExecutionView> SkillClient::GetExecution(const std::string& execution_id) {
  grpc::ClientContext context;
  Impl::ActiveCall active(*impl_, &context);
  impl_->Deadline(&context);
  api::GetExecutionRequest request;
  request.set_execution_id(execution_id);
  api::SkillExecution response;
  const auto status = impl_->stub->GetExecution(&context, request, &response);
  return status.ok() ? Result<ExecutionView>::Success(Execution(response))
                     : Failure<ExecutionView>(status);
}

Result<SignalReceipt> SkillClient::SignalExecution(const SignalAction& action) {
  grpc::ClientContext context;
  Impl::ActiveCall active(*impl_, &context);
  impl_->Deadline(&context);
  api::SignalExecutionRequest request;
  request.set_execution_id(action.execution_id);
  request.set_request_id(action.request_id);
  request.set_name(action.name);
  if (!JsonStruct(action.payload, request.mutable_payload()))
    return Result<SignalReceipt>::Failure(3, "signal payload must be a JSON object");
  api::SkillSignalReceipt response;
  const auto status = impl_->stub->SignalExecution(&context, request, &response);
  if (!status.ok()) return Failure<SignalReceipt>(status);
  return Result<SignalReceipt>::Success(
      {response.signal_id(), response.execution_id(), response.request_id(), response.accepted()});
}

Result<ExecutionView> SkillClient::TerminateExecution(const std::string& execution_id) {
  grpc::ClientContext context;
  Impl::ActiveCall active(*impl_, &context);
  impl_->Deadline(&context);
  api::TerminateExecutionRequest request;
  request.set_execution_id(execution_id);
  api::SkillExecution response;
  const auto status = impl_->stub->TerminateExecution(&context, request, &response);
  return status.ok() ? Result<ExecutionView>::Success(Execution(response))
                     : Failure<ExecutionView>(status);
}

Result<TerminateExecutionsReceipt> SkillClient::TerminateExecutions(
    const TerminateExecutionsAction& action) {
  grpc::ClientContext context;
  Impl::ActiveCall active(*impl_, &context);
  impl_->Deadline(&context);
  api::TerminateExecutionsRequest request;
  Reference(action.reference, request.mutable_reference());
  request.set_request_id(action.request_id);
  api::TerminateExecutionsResponse response;
  const auto status = impl_->stub->TerminateExecutions(&context, request, &response);
  if (!status.ok()) return Failure<TerminateExecutionsReceipt>(status);
  TerminateExecutionsReceipt result;
  result.request_id = response.request_id();
  result.execution_ids.assign(response.execution_ids().begin(), response.execution_ids().end());
  return Result<TerminateExecutionsReceipt>::Success(std::move(result));
}

TransportError SkillClient::FollowExecutionJournal(const std::string& execution_id,
                                                   std::uint64_t after_sequence,
                                                   const JournalHandler& handler) {
  grpc::ClientContext context;
  Impl::ActiveCall active(*impl_, &context);
  api::FollowExecutionJournalRequest request;
  request.set_execution_id(execution_id);
  request.set_after_journal_sequence(after_sequence);
  auto reader = impl_->stub->FollowExecutionJournal(&context, request);
  api::SkillJournalEntry entry;
  while (reader->Read(&entry)) {
    if (!handler(JournalEntry(entry))) {
      context.TryCancel();
      break;
    }
  }
  const auto status = reader->Finish();
  return status.ok() ? TransportError{} : Error(status);
}

TransportError SkillClient::MonitorExecutions(const ExecutionSelector& selector,
                                              std::uint64_t after_sequence,
                                              const MonitorHandler& handler) {
  grpc::ClientContext context;
  Impl::ActiveCall active(*impl_, &context);
  api::MonitorExecutionsRequest request;
  if (selector.reference)
    Reference(*selector.reference, request.mutable_selector()->mutable_reference());
  for (const auto status : selector.statuses)
    request.mutable_selector()->add_statuses(Status(status));
  request.set_after_monitor_sequence(after_sequence);
  auto reader = impl_->stub->MonitorExecutions(&context, request);
  api::SkillMonitorUpdate update;
  while (reader->Read(&update)) {
    if (!handler(MonitorUpdate(update))) {
      context.TryCancel();
      break;
    }
  }
  const auto status = reader->Finish();
  return status.ok() ? TransportError{} : Error(status);
}

void SkillClient::CancelPendingCalls() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  for (auto* context : impl_->active) context->TryCancel();
}

}  // namespace eos::ext::decision
