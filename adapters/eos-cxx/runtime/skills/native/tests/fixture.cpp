/// Test-only externally authored native skill library.
#include <chrono>
#include <cstddef>
#include <thread>

#include "eos/skill/abi.h"

namespace {

template <std::size_t Size>
constexpr eos_skill_text_v2 Text(const char (&value)[Size]) {
  return {value, static_cast<uint32_t>(Size - 1)};
}

eos_skill_execute_status_v2 Sequence(void*, const eos_skill_execution_v2* execution,
                                     eos_skill_complete_v2 complete, void* completion_context) {
  const eos_skill_primitive_request_v2 request{sizeof(eos_skill_primitive_request_v2),
                                               Text("base.posture"), Text("1.0.0"), Text("base-0"),
                                               Text(R"({"posture":"walk"})")};
  eos_skill_primitive_response_v2 first{sizeof(eos_skill_primitive_response_v2), {}, {}};
  eos_skill_primitive_response_v2 second{sizeof(eos_skill_primitive_response_v2), {}, {}};
  const eos_skill_host_status_v2 first_status =
      execution->host->call_primitive(execution->host->context, &request, &first);
  const eos_skill_host_status_v2 second_status =
      execution->host->call_primitive(execution->host->context, &request, &second);
  const bool accepted = first_status == EOS_SKILL_HOST_OK && second_status == EOS_SKILL_HOST_OK;
  const eos_skill_text_v2 reason = accepted ? Text("completed") : Text("host.rejected");
  const eos_skill_completion_v2 completion{sizeof(eos_skill_completion_v2), accepted ? 1u : 0u,
                                           reason, Text(R"({"fixture":true})")};
  complete(completion_context, &completion);
  return EOS_SKILL_EXECUTE_OK;
}

eos_skill_execute_status_v2 Async(void*, const eos_skill_execution_v2* execution,
                                  eos_skill_complete_v2 complete, void* completion_context) {
  if (!EOS_SKILL_HOST_V2_HAS(execution->host, start_primitive) ||
      !EOS_SKILL_HOST_V2_HAS(execution->host, wait_primitive) ||
      execution->host->start_primitive == nullptr || execution->host->wait_primitive == nullptr) {
    return EOS_SKILL_EXECUTE_FAILED;
  }
  const eos_skill_primitive_request_v2 request{sizeof(eos_skill_primitive_request_v2),
                                               Text("base.posture"), Text("1.0.0"), Text("base-0"),
                                               Text(R"({"posture":"walk"})")};
  eos_skill_primitive_start_response_v2 started{sizeof(started), 0, {}};
  if (execution->host->start_primitive(execution->host->context, &request, &started) !=
      EOS_SKILL_HOST_OK) {
    return EOS_SKILL_EXECUTE_FAILED;
  }
  eos_skill_primitive_wait_response_v2 waited{sizeof(waited), 0, {}, {}};
  if (execution->host->wait_primitive(execution->host->context, started.operation_id, 1000,
                                      &waited) != EOS_SKILL_HOST_OK ||
      waited.completed != 1) {
    return EOS_SKILL_EXECUTE_FAILED;
  }
  const eos_skill_completion_v2 completion{sizeof(eos_skill_completion_v2), 1, Text("completed"),
                                            Text(R"({"fixture":true})")};
  complete(completion_context, &completion);
  return EOS_SKILL_EXECUTE_OK;
}

bool StartAsync(const eos_skill_execution_v2* execution,
                eos_skill_primitive_start_response_v2* started) {
  const eos_skill_primitive_request_v2 request{sizeof(eos_skill_primitive_request_v2),
                                               Text("base.posture"), Text("1.0.0"), Text("base-0"),
                                               Text(R"({"posture":"walk"})")};
  return execution->host->start_primitive(execution->host->context, &request, started) ==
         EOS_SKILL_HOST_OK;
}

eos_skill_execute_status_v2 ConcurrentAsync(void*, const eos_skill_execution_v2* execution,
                                             eos_skill_complete_v2 complete,
                                             void* completion_context) {
  eos_skill_primitive_start_response_v2 first{sizeof(first), 0, {}};
  eos_skill_primitive_start_response_v2 second{sizeof(second), 0, {}};
  if (!StartAsync(execution, &first) || !StartAsync(execution, &second) ||
      first.operation_id == second.operation_id) {
    return EOS_SKILL_EXECUTE_FAILED;
  }

  const eos_skill_primitive_request_v2 request{sizeof(eos_skill_primitive_request_v2),
                                               Text("base.posture"), Text("1.0.0"), Text("base-0"),
                                               Text(R"({"posture":"walk"})")};
  eos_skill_primitive_response_v2 synchronous{sizeof(synchronous), {}, {}};
  if (execution->host->call_primitive(execution->host->context, &request, &synchronous) ==
          EOS_SKILL_HOST_OK ||
      execution->host->cancel_primitive(execution->host->context, first.operation_id) !=
          EOS_SKILL_HOST_OK) {
    return EOS_SKILL_EXECUTE_FAILED;
  }

  eos_skill_primitive_wait_response_v2 second_wait{sizeof(second_wait), 0, {}, {}};
  eos_skill_primitive_wait_response_v2 first_wait{sizeof(first_wait), 0, {}, {}};
  if (execution->host->wait_primitive(execution->host->context, second.operation_id, 1000,
                                      &second_wait) != EOS_SKILL_HOST_OK ||
      second_wait.completed != 1 ||
      execution->host->wait_primitive(execution->host->context, first.operation_id, 1000,
                                      &first_wait) != EOS_SKILL_HOST_OK ||
      first_wait.completed != 1) {
    return EOS_SKILL_EXECUTE_FAILED;
  }
  const eos_skill_completion_v2 completion{sizeof(completion), 1, Text("completed"),
                                            Text(R"({"fixture":true})")};
  complete(completion_context, &completion);
  return EOS_SKILL_EXECUTE_OK;
}

eos_skill_execute_status_v2 AbandonAsync(void*, const eos_skill_execution_v2* execution,
                                          eos_skill_complete_v2 complete,
                                          void* completion_context) {
  eos_skill_primitive_start_response_v2 first{sizeof(first), 0, {}};
  eos_skill_primitive_start_response_v2 second{sizeof(second), 0, {}};
  if (!StartAsync(execution, &first) || !StartAsync(execution, &second))
    return EOS_SKILL_EXECUTE_FAILED;
  const eos_skill_completion_v2 completion{sizeof(completion), 1, Text("completed"),
                                            Text(R"({"fixture":true})")};
  complete(completion_context, &completion);
  return EOS_SKILL_EXECUTE_OK;
}

eos_skill_execute_status_v2 InvalidArguments(void*, const eos_skill_execution_v2*,
                                             eos_skill_complete_v2, void*) {
  return EOS_SKILL_EXECUTE_INVALID_ARGUMENT;
}

eos_skill_execute_status_v2 ReportedFailure(void*, const eos_skill_execution_v2* execution,
                                            eos_skill_complete_v2, void*) {
  const eos_skill_failure_v2 failure{sizeof(failure), Text("fixture.backend_failed"),
                                     Text("fixture backend was unavailable")};
  execution->host->report_failure(execution->host->context, &failure);
  return EOS_SKILL_EXECUTE_FAILED;
}

eos_skill_execute_status_v2 SignalFailure(void*, const eos_skill_execution_v2* execution,
                                          eos_skill_complete_v2, void*) {
  const eos_skill_signal_v2 signal{sizeof(signal), Text("fixture.emitted"), Text("{}")};
  execution->host->emit_signal(execution->host->context, &signal);
  return EOS_SKILL_EXECUTE_FAILED;
}

eos_skill_execute_status_v2 Cancelled(void*, const eos_skill_execution_v2* execution,
                                      eos_skill_complete_v2, void*) {
  return execution->host->is_cancelled(execution->host->context) != 0 ? EOS_SKILL_EXECUTE_CANCELLED
                                                                      : EOS_SKILL_EXECUTE_FAILED;
}

eos_skill_execute_status_v2 InvalidResult(void*, const eos_skill_execution_v2*,
                                          eos_skill_complete_v2 complete,
                                          void* completion_context) {
  const eos_skill_completion_v2 completion{sizeof(eos_skill_completion_v2), 0, Text("rejected"),
                                           Text(R"({"wrong":true})")};
  complete(completion_context, &completion);
  return EOS_SKILL_EXECUTE_OK;
}

eos_skill_execute_status_v2 Pure(void*, const eos_skill_execution_v2*,
                                 eos_skill_complete_v2 complete, void* completion_context) {
  const eos_skill_completion_v2 completion{EOS_SKILL_COMPLETION_V2_BASE_SIZE, 1, Text("completed"),
                                           Text(R"({"fixture":true})")};
  complete(completion_context, &completion);
  return EOS_SKILL_EXECUTE_OK;
}

eos_skill_execute_status_v2 Uncooperative(void*, const eos_skill_execution_v2*,
                                          eos_skill_complete_v2 complete,
                                          void* completion_context) {
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const eos_skill_completion_v2 completion{sizeof(eos_skill_completion_v2), 1, Text("completed"),
                                           Text(R"({"fixture":true})")};
  complete(completion_context, &completion);
  return EOS_SKILL_EXECUTE_OK;
}

eos_skill_execute_status_v2 Blocking(void*, const eos_skill_execution_v2* execution,
                                     eos_skill_complete_v2 complete, void* completion_context) {
  const eos_skill_resource_request_v2 request{sizeof(eos_skill_resource_request_v2),
                                              Text("fixture:block")};
  eos_skill_resource_response_v2 response{sizeof(eos_skill_resource_response_v2), {}, {}};
  if (execution->host->read_resource(execution->host->context, &request, &response) !=
      EOS_SKILL_HOST_OK)
    return EOS_SKILL_EXECUTE_FAILED;
  const eos_skill_completion_v2 completion{sizeof(eos_skill_completion_v2), 1, Text("completed"),
                                           Text(R"({"fixture":true})")};
  complete(completion_context, &completion);
  return EOS_SKILL_EXECUTE_OK;
}

const eos_skill_primitive_reference_v2 kPrimitives[] = {
    {sizeof(eos_skill_primitive_reference_v2), Text("base.posture"), Text("1.0.0")}};
const eos_skill_resource_v2 kResources[] = {
    {sizeof(eos_skill_resource_v2), Text("fixture:block"), Text("Blocking test resource")}};
const eos_skill_signal_descriptor_v2 kSignals[] = {
    {sizeof(eos_skill_signal_descriptor_v2), Text("fixture.emitted"), Text(R"({"type":"object"})"),
     EOS_SKILL_SIGNAL_EMITTED}};
constexpr auto kParametersSchema = Text(
    R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","additionalProperties":false})");
constexpr auto kReturnsSchema = Text(
    R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","properties":{"fixture":{"type":"boolean"}},"required":["fixture"],"additionalProperties":false})");
const eos_skill_descriptor_v2 kSkills[] = {
    {sizeof(eos_skill_descriptor_v2), Text("fixture.abandon_async"), Text("1.0.0"),
     Text("Returns with two asynchronous primitives outstanding."), kParametersSchema,
     kReturnsSchema, 1, kPrimitives, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, AbandonAsync},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.async"), Text("1.0.0"),
     Text("Starts and waits for one asynchronous primitive."), kParametersSchema, kReturnsSchema, 1,
     kPrimitives, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, Async},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.concurrent_async"), Text("1.0.0"),
     Text("Controls two asynchronous primitives independently."), kParametersSchema,
     kReturnsSchema, 1, kPrimitives, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr,
     ConcurrentAsync},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.blocking"), Text("1.0.0"),
     Text("Blocks in a resource provider for concurrency tests."), kParametersSchema,
     kReturnsSchema, 0, nullptr, 1, kResources, 0, nullptr, 0, nullptr, 0, nullptr, Blocking},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.sequence"), Text("1.0.0"),
     Text("Calls two existing primitives through the EOS host."), kParametersSchema, kReturnsSchema,
     1, kPrimitives, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, Sequence},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.invalid"), Text("1.0.0"),
     Text("Rejects invocation arguments."), kParametersSchema, kReturnsSchema, 1, kPrimitives, 0,
     nullptr, 0, nullptr, 0, nullptr, 0, nullptr, InvalidArguments},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.reported_failure"), Text("1.0.0"),
     Text("Reports a terminal failure diagnostic."), kParametersSchema, kReturnsSchema, 0, nullptr,
     0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, ReportedFailure},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.signal_failure"), Text("1.0.0"),
     Text("Fails after signal delivery is unavailable."), kParametersSchema, kReturnsSchema, 0,
     nullptr, 0, nullptr, 1, kSignals, 0, nullptr, 0, nullptr, SignalFailure},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.invalid_result"), Text("1.0.0"),
     Text("Returns a value outside its declared schema."), kParametersSchema, kReturnsSchema, 1,
     kPrimitives, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, InvalidResult},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.cancel"), Text("1.0.0"),
     Text("Observes host cancellation."), kParametersSchema, kReturnsSchema, 1, kPrimitives, 0,
     nullptr, 0, nullptr, 0, nullptr, 0, nullptr, Cancelled},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.pure"), Text("1.0.0"),
     Text("Runs without physical-effect capabilities."), kParametersSchema, kReturnsSchema, 0,
     nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, Pure},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.uncooperative"), Text("1.0.0"),
     Text("Ignores cancellation while it completes."), kParametersSchema, kReturnsSchema, 0,
     nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, Uncooperative},
};
const eos_skill_library_v2 kLibrary = {EOS_SKILL_ABI_VERSION_V2,
                                       sizeof(eos_skill_library_v2),
                                       Text("fixture"),
                                       Text("1.0.0"),
                                       Text("test-build"),
                                        12,
                                        kSkills,
                                        sizeof(eos_skill_descriptor_v2)};

}  // namespace

extern "C" EOS_SKILL_EXPORT const eos_skill_library_v2* eos_skill_library_query_v2(
    uint32_t host_abi_version) {
  return host_abi_version == EOS_SKILL_ABI_VERSION_V2 ? &kLibrary : nullptr;
}
