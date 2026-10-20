#include <cstring>

#include "eos/skill/abi.h"

namespace {
template <std::size_t Size>
constexpr eos_skill_text_v2 Text(const char (&value)[Size]) {
  return {value, Size - 1};
}

eos_skill_execute_status_v2 Execute(void*, const eos_skill_execution_v2* execution,
                                    eos_skill_complete_v2 complete, void* completion_context) {
  if (execution == nullptr || execution->host == nullptr ||
      execution->host->abi_version != EOS_SKILL_ABI_VERSION_V2)
    return EOS_SKILL_EXECUTE_INVALID_ARGUMENT;
  if (EOS_SKILL_HOST_V2_HAS(execution->host, log) && execution->host->log != nullptr) {
    const eos_skill_log_record_v2 record{sizeof(record), EOS_SKILL_LOG_INFO,
                                         Text("fixture_started"), Text("Fixture running"),
                                         Text("{\"phase\":\"test\"}")};
    if (execution->host->log(execution->host->context, &record) != EOS_SKILL_HOST_OK)
      return EOS_SKILL_EXECUTE_FAILED;
  }
  if (EOS_SKILL_HOST_V2_HAS(execution->host, emit_checkpoint) &&
      execution->host->emit_checkpoint != nullptr) {
    const eos_skill_checkpoint_v2 checkpoint{sizeof(checkpoint), Text("fixture.running"),
                                              Text("Fixture running"), 1u, 0.5,
                                              Text("{\"phase\":\"test\"}")};
    (void)execution->host->emit_checkpoint(execution->host->context, &checkpoint);
  }
  eos_skill_signal_v2 received{sizeof(eos_skill_signal_v2), {}, {}};
  const eos_skill_signal_receive_status_v2 receive_status =
      execution->host->receive_signal(execution->host->context, 0, &received);
  if (receive_status == EOS_SKILL_SIGNAL_RECEIVED) {
    eos_skill_signal_v2 emitted{sizeof(eos_skill_signal_v2), Text("progress.changed"),
                                received.payload_json};
    if (execution->host->emit_signal(execution->host->context, &emitted) != EOS_SKILL_HOST_OK)
      return EOS_SKILL_EXECUTE_FAILED;
  } else if (receive_status == EOS_SKILL_SIGNAL_RECEIVE_INTERNAL) {
    return EOS_SKILL_EXECUTE_FAILED;
  }
  const eos_skill_completion_v2 result{sizeof(eos_skill_completion_v2), 1, Text("completed"),
                                       Text("{}")};
  return complete(completion_context, &result) == EOS_SKILL_HOST_OK ? EOS_SKILL_EXECUTE_OK
                                                                    : EOS_SKILL_EXECUTE_FAILED;
}

eos_skill_execute_status_v2 SourceRead(void*, const eos_skill_execution_v2* execution,
                                       eos_skill_complete_v2 complete, void* completion_context) {
  eos_skill_source_query_v2 query{sizeof(query), Text("telemetry.speed"),
                                   EOS_SKILL_SOURCE_LATEST, 0};
  eos_skill_source_response_v2 response{sizeof(response), 7, {7, Text("dirty"), 1, 1,
                                                               Text("dirty")}, Text("dirty")};
  if (execution->host->query_source(execution->host->context, &query, &response) !=
          EOS_SKILL_HOST_OK ||
      response.has_sample != 1 || response.sample.struct_size != sizeof(response.sample))
    return EOS_SKILL_EXECUTE_FAILED;
  const eos_skill_completion_v2 result{sizeof(result), 1, Text("completed"), Text("{}")};
  return complete(completion_context, &result) == EOS_SKILL_HOST_OK ? EOS_SKILL_EXECUTE_OK
                                                                    : EOS_SKILL_EXECUTE_FAILED;
}

eos_skill_execute_status_v2 SourceUnavailable(void*, const eos_skill_execution_v2* execution,
                                              eos_skill_complete_v2 complete,
                                              void* completion_context) {
  eos_skill_source_query_v2 query{sizeof(query), Text("telemetry.speed"),
                                   EOS_SKILL_SOURCE_LATEST, 0};
  eos_skill_source_response_v2 response{sizeof(response), 7, {7, Text("dirty"), 1, 1,
                                                               Text("dirty")}, Text("dirty")};
  if (execution->host->query_source(execution->host->context, &query, &response) !=
          EOS_SKILL_HOST_UNAVAILABLE ||
      response.has_sample != 0 || response.sample.struct_size != sizeof(response.sample) ||
      response.sample.topic.data != nullptr || response.sample.value_json.data != nullptr ||
      response.error_message.data != nullptr)
    return EOS_SKILL_EXECUTE_FAILED;
  const eos_skill_completion_v2 result{sizeof(result), 1, Text("completed"), Text("{}")};
  return complete(completion_context, &result) == EOS_SKILL_HOST_OK ? EOS_SKILL_EXECUTE_OK
                                                                    : EOS_SKILL_EXECUTE_FAILED;
}

eos_skill_execute_status_v2 SourceUnauthorized(void*, const eos_skill_execution_v2* execution,
                                               eos_skill_complete_v2 complete,
                                               void* completion_context) {
  eos_skill_source_sample_v2 sample{sizeof(sample), Text("telemetry.speed"), 0, 0,
                                     Text("{\"speed\":3}")};
  eos_skill_source_response_v2 response{sizeof(response), 0, {}, {}};
  if (execution->host->publish_source(execution->host->context, &sample, &response) !=
      EOS_SKILL_HOST_INVALID_ARGUMENT)
    return EOS_SKILL_EXECUTE_FAILED;
  const eos_skill_completion_v2 result{sizeof(result), 1, Text("completed"), Text("{}")};
  return complete(completion_context, &result) == EOS_SKILL_HOST_OK ? EOS_SKILL_EXECUTE_OK
                                                                    : EOS_SKILL_EXECUTE_FAILED;
}

eos_skill_execute_status_v2 AsyncCancellation(void*, const eos_skill_execution_v2* execution,
                                              eos_skill_complete_v2, void*) {
  const eos_skill_primitive_request_v2 request{
      sizeof(request), Text("base.posture"), Text("1.0.0"), Text("base-0"),
      Text(R"({"posture":"walk"})")};
  eos_skill_primitive_start_response_v2 started{sizeof(started), 0, {}};
  if (execution->host->start_primitive(execution->host->context, &request, &started) !=
      EOS_SKILL_HOST_OK)
    return EOS_SKILL_EXECUTE_FAILED;
  eos_skill_primitive_wait_response_v2 waited{sizeof(waited), 0, {}, {}};
  const eos_skill_host_status_v2 status = execution->host->wait_primitive(
      execution->host->context, started.operation_id, 10000, &waited);
  return status == EOS_SKILL_HOST_UNAVAILABLE &&
                 execution->host->is_cancelled(execution->host->context) != 0
             ? EOS_SKILL_EXECUTE_CANCELLED
             : EOS_SKILL_EXECUTE_FAILED;
}

eos_skill_execute_status_v2 Inference(void*, const eos_skill_execution_v2* execution,
                                       eos_skill_complete_v2 complete, void* completion_context) {
  eos_skill_inference_request_v2 invalid_vad{
      sizeof(invalid_vad), EOS_SKILL_INFERENCE_VAD, Text("microphone.front"), 1, 1,
      Text("{\"input\":{\"data_b64\":\"AAAA\"}}")};
  eos_skill_inference_response_v2 invalid_response{sizeof(invalid_response), {}, {}};
  if (execution->host->infer_once(execution->host->context, &invalid_vad, &invalid_response) !=
      EOS_SKILL_HOST_INVALID_ARGUMENT)
    return EOS_SKILL_EXECUTE_FAILED;
  eos_skill_inference_request_v2 vad{
      sizeof(vad), EOS_SKILL_INFERENCE_VAD, {}, 0, 0,
      Text("{\"input\":{\"data_b64\":\"AAAA\",\"sample_rate_hz\":16000,\"channels\":1}}")};
  eos_skill_inference_response_v2 vad_response{sizeof(vad_response), {}, {}};
  if (execution->host->infer_once(execution->host->context, &vad, &vad_response) !=
      EOS_SKILL_HOST_OK)
    return EOS_SKILL_EXECUTE_FAILED;
  eos_skill_inference_request_v2 mixed{
      sizeof(mixed), EOS_SKILL_INFERENCE_HUMAN_POSE, Text("camera.front"), 12, 9,
      Text("{\"input\":{\"data_b64\":\"AAAA\"}}")};
  eos_skill_inference_response_v2 mixed_response{sizeof(mixed_response), {}, {}};
  if (execution->host->infer_once(execution->host->context, &mixed, &mixed_response) !=
      EOS_SKILL_HOST_INVALID_ARGUMENT)
    return EOS_SKILL_EXECUTE_FAILED;
  eos_skill_inference_request_v2 inline_request{
      sizeof(inline_request), EOS_SKILL_INFERENCE_HUMAN_POSE, {}, 0, 0,
      Text("{\"input\":{\"data_b64\":\"AAAA\"}}")};
  eos_skill_inference_response_v2 inline_response{sizeof(inline_response), {}, {}};
  if (execution->host->infer_once(execution->host->context, &inline_request, &inline_response) !=
      EOS_SKILL_HOST_OK)
    return EOS_SKILL_EXECUTE_FAILED;
  eos_skill_inference_request_v2 request{sizeof(request), EOS_SKILL_INFERENCE_HUMAN_POSE,
                                          Text("camera.front"), 12, 9,
                                          Text("{\"human_id\":\"human:7\"}")};
  eos_skill_inference_response_v2 response{sizeof(response), {}, {}};
  if (execution->host->infer_once(execution->host->context, &request, &response) !=
      EOS_SKILL_HOST_OK)
    return EOS_SKILL_EXECUTE_FAILED;
  const eos_skill_completion_v2 result{sizeof(result), 1, Text("completed"),
                                        response.result_json};
  return complete(completion_context, &result) == EOS_SKILL_HOST_OK ? EOS_SKILL_EXECUTE_OK
                                                                    : EOS_SKILL_EXECUTE_FAILED;
}

eos_skill_execute_status_v2 UndeclaredInference(void*, const eos_skill_execution_v2* execution,
                                                eos_skill_complete_v2 complete,
                                                void* completion_context) {
  eos_skill_inference_request_v2 request{sizeof(request), EOS_SKILL_INFERENCE_HUMAN_POSE,
                                          Text("camera.front"), 12, 9, Text("{}")};
  eos_skill_inference_response_v2 response{sizeof(response), {}, {}};
  if (execution->host->infer_once(execution->host->context, &request, &response) !=
      EOS_SKILL_HOST_INVALID_ARGUMENT)
    return EOS_SKILL_EXECUTE_FAILED;
  const eos_skill_completion_v2 result{sizeof(result), 1, Text("completed"), Text("{}")};
  return complete(completion_context, &result) == EOS_SKILL_HOST_OK ? EOS_SKILL_EXECUTE_OK
                                                                    : EOS_SKILL_EXECUTE_FAILED;
}

constexpr char kObjectSchema[] = R"({"type":"object"})";
constexpr char kSignalSchema[] =
    R"({"type":"object","properties":{"speed":{"type":"integer"}},"required":["speed"],"additionalProperties":false})";
constexpr eos_skill_signal_descriptor_v2 kSignals[] = {
    {sizeof(eos_skill_signal_descriptor_v2), Text("control.update"), Text(kSignalSchema),
      EOS_SKILL_SIGNAL_ACCEPTED},
    {sizeof(eos_skill_signal_descriptor_v2), Text("progress.changed"), Text(kSignalSchema),
      EOS_SKILL_SIGNAL_EMITTED}};
constexpr eos_skill_claim_template_v2 kClaims[] = {
    {sizeof(eos_skill_claim_template_v2), EOS_SKILL_CLAIM_RESOURCE, EOS_SKILL_CLAIM_EXCLUSIVE_USE,
      Text("/resource_id")}};
constexpr char kSourceSchema[] =
    R"({"type":"object","properties":{"speed":{"type":"integer"}},"required":["speed"],"additionalProperties":false})";
constexpr eos_skill_source_descriptor_v2 kReadSource[] = {
    {sizeof(eos_skill_source_descriptor_v2), Text("telemetry.speed"), Text(kSourceSchema),
     EOS_SKILL_SOURCE_READ}};
constexpr eos_skill_primitive_reference_v2 kAsyncPrimitive[] = {
    {sizeof(eos_skill_primitive_reference_v2), Text("base.posture"), Text("1.0.0")}};
constexpr eos_skill_inference_descriptor_v2 kPoseInference[] = {
    {sizeof(eos_skill_inference_descriptor_v2), EOS_SKILL_INFERENCE_VAD},
    {sizeof(eos_skill_inference_descriptor_v2), EOS_SKILL_INFERENCE_HUMAN_POSE}};
constexpr eos_skill_descriptor_v2 kSkills[] = {
    {sizeof(eos_skill_descriptor_v2), Text("fixture.signals"), Text("1.0.0"),
     Text("ABI v2 fixture"), Text(kObjectSchema), Text(kObjectSchema), 0, nullptr, 0, nullptr, 2,
      kSignals, 1, kClaims, 1, nullptr, Execute, 0, nullptr},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.source_read"), Text("1.0.0"),
     Text("Reads a declared source"), Text(kObjectSchema), Text(kObjectSchema), 0, nullptr, 0,
     nullptr, 0, nullptr, 0, nullptr, 1, nullptr, SourceRead, 1, kReadSource},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.source_unavailable"), Text("1.0.0"),
     Text("Checks initialized source responses"), Text(kObjectSchema), Text(kObjectSchema), 0,
     nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 1, nullptr, SourceUnavailable, 1, kReadSource},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.source_unauthorized"), Text("1.0.0"),
     Text("Attempts an unauthorized source write"), Text(kObjectSchema), Text(kObjectSchema), 0,
     nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 1, nullptr, SourceUnauthorized, 1,
     kReadSource},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.async_cancellation"), Text("1.0.0"),
     Text("Waits for cancellation of an asynchronous primitive"), Text(kObjectSchema),
     Text(kObjectSchema), 1, kAsyncPrimitive, 0, nullptr, 0, nullptr, 0, nullptr, 1, nullptr,
      AsyncCancellation, 0, nullptr},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.inference"), Text("1.0.0"),
     Text("Runs declared retained-frame inference"), Text(kObjectSchema), Text(kObjectSchema), 0,
      nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 1, nullptr, Inference, 0, nullptr, 2,
     kPoseInference},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.inference_undeclared"), Text("1.0.0"),
     Text("Attempts undeclared inference"), Text(kObjectSchema), Text(kObjectSchema), 0, nullptr,
     0, nullptr, 0, nullptr, 0, nullptr, 1, nullptr, UndeclaredInference, 0, nullptr, 0,
     nullptr}};
constexpr eos_skill_library_v2 kLibrary = {EOS_SKILL_ABI_VERSION_V2,
                                           sizeof(eos_skill_library_v2),
                                           Text("fixture.v2"),
                                           Text("1.0.0"),
                                           Text("test"),
                                             7,
                                            kSkills,
                                            sizeof(eos_skill_descriptor_v2)};
}  // namespace

extern "C" EOS_SKILL_EXPORT const eos_skill_library_v2* eos_skill_library_query_v2(uint32_t abi) {
  return abi == EOS_SKILL_ABI_VERSION_V2 ? &kLibrary : nullptr;
}
