#include "src/eos/runtime/northbound/skill_codec.h"

#include <google/protobuf/util/json_util.h>

#include <chrono>
#include <stdexcept>

namespace eos::runtime::northbound {
namespace {

island::eos::v1::ExecutionStatus ProtoStatus(skills::ExecutionStatus status) {
  using Proto = island::eos::v1::ExecutionStatus;
  switch (status) {
    case skills::ExecutionStatus::kQueued:
      return Proto::EXECUTION_STATUS_QUEUED;
    case skills::ExecutionStatus::kRunning:
      return Proto::EXECUTION_STATUS_RUNNING;
    case skills::ExecutionStatus::kSucceeded:
      return Proto::EXECUTION_STATUS_SUCCEEDED;
    case skills::ExecutionStatus::kRejected:
      return Proto::EXECUTION_STATUS_REJECTED;
    case skills::ExecutionStatus::kCancelled:
      return Proto::EXECUTION_STATUS_CANCELLED;
    case skills::ExecutionStatus::kTimedOut:
      return Proto::EXECUTION_STATUS_TIMED_OUT;
    case skills::ExecutionStatus::kFailed:
      return Proto::EXECUTION_STATUS_FAILED;
  }
  return Proto::EXECUTION_STATUS_UNSPECIFIED;
}

island::eos::v1::SkillClaimKind ProtoClaimKind(skills::ClaimKind kind) {
  switch (kind) {
    case skills::ClaimKind::kDevice:
      return island::eos::v1::SKILL_CLAIM_KIND_DEVICE;
    case skills::ClaimKind::kResource:
      return island::eos::v1::SKILL_CLAIM_KIND_RESOURCE;
    case skills::ClaimKind::kProviderContext:
      return island::eos::v1::SKILL_CLAIM_KIND_PROVIDER_CONTEXT;
  }
  return island::eos::v1::SKILL_CLAIM_KIND_UNSPECIFIED;
}

island::eos::v1::SkillClaimMode ProtoClaimMode(skills::ClaimMode mode) {
  return mode == skills::ClaimMode::kSharedRead ? island::eos::v1::SKILL_CLAIM_MODE_SHARED_READ
                                                : island::eos::v1::SKILL_CLAIM_MODE_EXCLUSIVE_USE;
}

island::eos::v1::SkillSourceAuthority ProtoSourceAuthority(
    skills::SourceAuthority authority) {
  return authority == skills::SourceAuthority::kWrite
             ? island::eos::v1::SKILL_SOURCE_AUTHORITY_WRITE
             : island::eos::v1::SKILL_SOURCE_AUTHORITY_READ;
}

void FillTimestamp(std::chrono::system_clock::time_point value,
                    google::protobuf::Timestamp* output) {
  const auto since_epoch = value.time_since_epoch();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
  auto seconds_count = seconds.count();
  auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch - seconds).count();
  if (nanos < 0) {
    --seconds_count;
    nanos += 1000000000;
  }
  output->set_seconds(seconds_count);
  output->set_nanos(static_cast<std::int32_t>(nanos));
}

void FillDuration(std::chrono::milliseconds value, google::protobuf::Duration* output) {
  if (value.count() < 0) value = std::chrono::milliseconds(0);
  output->set_seconds(value.count() / 1000);
  output->set_nanos(static_cast<std::int32_t>((value.count() % 1000) * 1000000));
}

void FillEventTime(const skills::EventTime& value, island::eos::v1::SkillEventTime* output) {
  if (value.source_timestamp_ns) output->set_source_timestamp_ns(*value.source_timestamp_ns);
  output->set_source_clock_id(value.source_clock_id);
  FillTimestamp(value.observed_at, output->mutable_observed_at());
  output->set_observed_monotonic_ns(value.observed_monotonic_ns);
  output->set_boot_id(value.boot_id);
  output->set_sequence(value.sequence);
  output->set_event_id(value.event_id);
}

island::eos::v1::SkillJournalSource ProtoJournalSource(skills::JournalSource source) {
  return source == skills::JournalSource::kSkill
             ? island::eos::v1::SKILL_JOURNAL_SOURCE_SKILL
             : island::eos::v1::SKILL_JOURNAL_SOURCE_SYSTEM;
}

island::eos::v1::SkillJournalKind ProtoJournalKind(skills::JournalKind kind) {
  switch (kind) {
    case skills::JournalKind::kActivity:
      return island::eos::v1::SKILL_JOURNAL_KIND_ACTIVITY;
    case skills::JournalKind::kCheckpoint:
      return island::eos::v1::SKILL_JOURNAL_KIND_CHECKPOINT;
    case skills::JournalKind::kSuppression:
      return island::eos::v1::SKILL_JOURNAL_KIND_SUPPRESSION;
  }
  return island::eos::v1::SKILL_JOURNAL_KIND_UNSPECIFIED;
}

}  // namespace

nlohmann::json SkillStructJson(const google::protobuf::Struct& value) {
  std::string text;
  const auto encoded = google::protobuf::util::MessageToJsonString(value, &text);
  if (!encoded.ok()) throw std::invalid_argument("JSON object cannot be decoded");
  return nlohmann::json::parse(text);
}

grpc::Status FillSkillStruct(const nlohmann::json& value, google::protobuf::Struct* output) {
  const auto status = google::protobuf::util::JsonStringToMessage(value.dump(), output);
  return status.ok() ? grpc::Status::OK
                      : grpc::Status(grpc::StatusCode::DATA_LOSS, "cannot encode JSON object");
}

island::eos::v1::SkillInferenceAuthority ProtoInferenceAuthority(
    skills::InferenceAuthority authority) {
  switch (authority) {
    case skills::InferenceAuthority::kVad:
      return island::eos::v1::SKILL_INFERENCE_AUTHORITY_VAD;
    case skills::InferenceAuthority::kHuman:
      return island::eos::v1::SKILL_INFERENCE_AUTHORITY_HUMAN;
    case skills::InferenceAuthority::kHumanPose:
      return island::eos::v1::SKILL_INFERENCE_AUTHORITY_HUMAN_POSE;
    case skills::InferenceAuthority::kFaceRecognition:
      return island::eos::v1::SKILL_INFERENCE_AUTHORITY_FACE_RECOGNITION;
    case skills::InferenceAuthority::kChatCompletion:
      return island::eos::v1::SKILL_INFERENCE_AUTHORITY_CHAT_COMPLETION;
    case skills::InferenceAuthority::kVlm:
      return island::eos::v1::SKILL_INFERENCE_AUTHORITY_VLM;
  }
  return island::eos::v1::SKILL_INFERENCE_AUTHORITY_UNSPECIFIED;
}

grpc::Status FillSkillDescriptor(const skills::Metadata& metadata,
                                 island::eos::v1::SkillDescriptor* output) {
  output->mutable_reference()->set_name(metadata.name);
  output->mutable_reference()->set_version(metadata.version);
  output->set_description(metadata.description);
  grpc::Status status = FillSkillStruct(metadata.parameters_schema, output->mutable_parameters());
  if (!status.ok()) return status;
  status = FillSkillStruct(metadata.returns_schema, output->mutable_returns());
  if (!status.ok()) return status;
  for (const auto& primitive : metadata.primitives) {
    auto* reference = output->add_primitives();
    reference->set_name(primitive.name);
    reference->set_version(primitive.version);
  }
  for (const auto& resource : metadata.resources) {
    auto* exposed = output->add_resources();
    exposed->set_uri_template(resource.uri_template);
    exposed->set_description(resource.description);
  }
  for (const auto& signal : metadata.accepted_signals) {
    auto* encoded = output->add_accepted_signals();
    encoded->set_name(signal.name);
    status = FillSkillStruct(signal.schema, encoded->mutable_payload_schema());
    if (!status.ok()) return status;
  }
  for (const auto& signal : metadata.emitted_signals) {
    auto* encoded = output->add_emitted_signals();
    encoded->set_name(signal.name);
    status = FillSkillStruct(signal.schema, encoded->mutable_payload_schema());
    if (!status.ok()) return status;
  }
  for (const auto& claim : metadata.claim_templates) {
    auto* encoded = output->add_claims();
    encoded->set_kind(ProtoClaimKind(claim.kind));
    encoded->set_mode(ProtoClaimMode(claim.mode));
    encoded->set_argument_json_pointer(claim.argument_pointer);
  }
  for (const auto& source : metadata.sources) {
    auto* encoded = output->add_sources();
    encoded->set_topic(source.topic);
    encoded->set_authority(ProtoSourceAuthority(source.authority));
    status = FillSkillStruct(source.schema, encoded->mutable_schema());
    if (!status.ok()) return status;
  }
  for (const auto authority : metadata.inference_authorities)
    output->add_inference_authorities(ProtoInferenceAuthority(authority));
  for (const auto& channel : metadata.ipc_channels) {
    auto* encoded = output->add_ipc_channels();
    encoded->set_name(channel.name);
    encoded->set_can_send(channel.can_send);
    encoded->set_can_receive(channel.can_receive);
    status = FillSkillStruct(channel.schema, encoded->mutable_payload_schema());
    if (!status.ok()) return status;
  }
  if (metadata.execution_state_schema) {
    status = FillSkillStruct(*metadata.execution_state_schema,
                             output->mutable_execution_state_schema());
    if (!status.ok()) return status;
  }
  output->set_reentrant(metadata.reentrant);
  return grpc::Status::OK;
}

grpc::Status FillSkillExecution(const skills::ExecutionSnapshot& snapshot,
                                 island::eos::v1::SkillExecution* output) {
  output->set_execution_id(snapshot.execution_id);
  output->set_request_id(snapshot.request_id);
  output->mutable_reference()->set_name(snapshot.skill_name);
  output->mutable_reference()->set_version(snapshot.skill_version);
  output->set_status(ProtoStatus(snapshot.status));
  output->set_sequence(snapshot.sequence);
  output->set_cancellation_requested(snapshot.cancellation_requested);
  output->set_reason_code(snapshot.reason_code);
  output->set_message(snapshot.message);
  output->set_queue_reason(snapshot.queue_reason);
  output->set_latest_journal_sequence(snapshot.latest_journal_sequence);
  output->set_runtime_id(snapshot.runtime_id);
  output->set_boot_id(snapshot.boot_id);
  output->set_parent_execution_id(snapshot.parent_execution_id);
  FillEventTime(snapshot.event_time, output->mutable_event_time());
  for (const auto& claim : snapshot.claims) {
    auto* encoded = output->add_claims();
    encoded->set_kind(ProtoClaimKind(claim.kind));
    encoded->set_mode(ProtoClaimMode(claim.mode));
    encoded->set_identity(claim.value);
  }
  for (const auto& claim : snapshot.active_device_claims) {
    auto* encoded = output->add_active_device_claims();
    encoded->set_kind(ProtoClaimKind(claim.kind));
    encoded->set_mode(ProtoClaimMode(claim.mode));
    encoded->set_identity(claim.value);
    auto* active = output->add_active_claims();
    active->set_kind(ProtoClaimKind(claim.kind));
    active->set_mode(ProtoClaimMode(claim.mode));
    active->set_identity(claim.value);
  }
  if (snapshot.created_at) FillTimestamp(*snapshot.created_at, output->mutable_created_at());
  if (snapshot.deadline_at) FillTimestamp(*snapshot.deadline_at, output->mutable_deadline_at());
  if (snapshot.started_at) FillTimestamp(*snapshot.started_at, output->mutable_started_at());
  if (snapshot.cancellation_requested_at)
    FillTimestamp(*snapshot.cancellation_requested_at,
                  output->mutable_cancellation_requested_at());
  if (snapshot.finished_at) FillTimestamp(*snapshot.finished_at, output->mutable_finished_at());
  FillDuration(snapshot.queue_duration, output->mutable_queue_duration());
  FillDuration(snapshot.run_duration, output->mutable_run_duration());
  FillDuration(snapshot.total_duration, output->mutable_total_duration());
  if (snapshot.current_activity) {
    auto* activity = output->mutable_current_activity();
    activity->set_activity_id(snapshot.current_activity->id);
    activity->set_name(snapshot.current_activity->name);
    FillTimestamp(snapshot.current_activity->started_at, activity->mutable_started_at());
    grpc::Status status =
        FillSkillStruct(snapshot.current_activity->fields, activity->mutable_fields());
    if (!status.ok()) return status;
  }
  auto* accounting = output->mutable_accounting();
  accounting->set_primitive_calls(snapshot.accounting.primitive_calls);
  accounting->set_resource_reads(snapshot.accounting.resource_reads);
  accounting->set_signals_received(snapshot.accounting.signals_received);
  accounting->set_signals_emitted(snapshot.accounting.signals_emitted);
  accounting->set_diagnostic_logs(snapshot.accounting.diagnostic_logs);
  accounting->set_diagnostic_logs_dropped(snapshot.accounting.diagnostic_logs_dropped);
  accounting->set_checkpoints(snapshot.accounting.checkpoints);
  accounting->set_checkpoints_dropped(snapshot.accounting.checkpoints_dropped);
  accounting->set_activity_events(snapshot.accounting.activity_events);
  accounting->set_activity_events_dropped(snapshot.accounting.activity_events_dropped);
  FillDuration(snapshot.accounting.claim_wait_duration,
               accounting->mutable_claim_wait_duration());
  FillDuration(snapshot.accounting.signal_wait_duration,
               accounting->mutable_signal_wait_duration());
  FillDuration(snapshot.accounting.capability_duration,
               accounting->mutable_capability_duration());
  return FillSkillStruct(snapshot.result, output->mutable_result());
}

grpc::Status FillExecutionState(const std::string& execution_id,
                                const skills::RetainedExecutionState& state,
                                island::eos::v1::SkillExecutionState* output) {
  output->set_execution_id(execution_id);
  output->set_revision(state.revision);
  FillTimestamp(state.published_at, output->mutable_published_at());
  return FillSkillStruct(state.payload, output->mutable_payload());
}

grpc::Status FillJournalEntry(const skills::JournalEntry& event,
                              island::eos::v1::SkillJournalEntry* output) {
  output->set_journal_sequence(event.sequence);
  FillTimestamp(event.timestamp, output->mutable_timestamp());
  switch (event.kind) {
    case skills::JournalEntry::Kind::kLifecycle:
      output->set_execution_id(event.execution.execution_id);
      return FillSkillExecution(event.execution, output->mutable_snapshot());
    case skills::JournalEntry::Kind::kSignal: {
      output->set_execution_id(event.signal.execution_id);
      auto* signal = output->mutable_signal();
      signal->set_signal_id(event.signal.signal_id);
      signal->set_execution_id(event.signal.execution_id);
      signal->set_direction(island::eos::v1::SKILL_SIGNAL_DIRECTION_SKILL_TO_AGENT);
      signal->set_name(event.signal.name);
      signal->set_journal_sequence(event.sequence);
      return FillSkillStruct(event.signal.payload, signal->mutable_payload());
    }
    case skills::JournalEntry::Kind::kRecord: {
      output->set_execution_id(event.execution.execution_id);
      auto* record = output->mutable_record();
      record->set_event_id(event.record.event_id);
      record->set_execution_id(event.execution.execution_id);
      record->set_journal_sequence(event.sequence);
      record->set_source(ProtoJournalSource(event.record.source));
      record->set_kind(ProtoJournalKind(event.record.kind));
      record->set_name(event.record.name);
      record->set_outcome(event.record.outcome);
      record->set_message(event.record.message);
      FillDuration(event.record.elapsed, record->mutable_elapsed());
      if (event.record.duration)
        FillDuration(*event.record.duration, record->mutable_duration());
      if (event.record.progress) record->set_progress(*event.record.progress);
      return FillSkillStruct(event.record.fields, record->mutable_fields());
    }
    case skills::JournalEntry::Kind::kGap: {
      output->set_execution_id(event.execution.execution_id);
      auto* gap = output->mutable_gap();
      gap->set_after_sequence(event.gap_after_sequence);
      gap->set_first_available_sequence(event.gap_first_available_sequence);
      return grpc::Status::OK;
    }
  }
  return grpc::Status(grpc::StatusCode::INTERNAL, "unknown execution event kind");
}

grpc::Status FillMonitorUpdate(const skills::MonitorUpdate& update,
                               island::eos::v1::SkillMonitorUpdate* output) {
  output->set_monitor_sequence(update.sequence);
  output->set_reset(update.reset);
  for (const auto& snapshot : update.upserted) {
    const auto status = FillSkillExecution(snapshot, output->add_upserted());
    if (!status.ok()) return status;
  }
  for (const auto& id : update.removed_execution_ids) output->add_removed_execution_ids(id);
  return grpc::Status::OK;
}

}  // namespace eos::runtime::northbound
