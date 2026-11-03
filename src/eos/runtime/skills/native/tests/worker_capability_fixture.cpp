#include <cstddef>
#include <cerrno>
#include <fcntl.h>
#include "eos/skill/abi.h"

namespace {
template <std::size_t N>
constexpr eos_skill_text_v2 Text(const char (&value)[N]) { return {value, N - 1}; }

eos_skill_execute_status_v2 Execute(void*, const eos_skill_execution_v2* execution,
                                    eos_skill_complete_v2 complete, void* context) {
  const eos_skill_primitive_request_v2 request{
      sizeof(request), Text("base.posture"), Text("1.0.0"), Text("default"),
      Text(R"({"posture":"walk"})")};
  eos_skill_primitive_response_v2 response{sizeof(response), {}, {}};
  if (execution->host->call_primitive(execution->host->context, &request, &response) !=
      EOS_SKILL_HOST_OK)
    return EOS_SKILL_EXECUTE_FAILED;
  const eos_skill_completion_v2 result{sizeof(result), 1, Text("completed"), Text("{}")};
  return complete(context, &result) == EOS_SKILL_HOST_OK ? EOS_SKILL_EXECUTE_OK
                                                         : EOS_SKILL_EXECUTE_FAILED;
}
eos_skill_execute_status_v2 ExecuteVelocity(void*, const eos_skill_execution_v2* execution,
                                            eos_skill_complete_v2 complete, void* context) {
  const eos_skill_primitive_request_v2 request{
      sizeof(request), Text("base.velocity"), Text("1.0.0"), Text("default"),
      Text(R"({"linear_x_m_s":0.1,"linear_y_m_s":0.0,"angular_z_rad_s":0.0,"valid_for_ms":1000})")};
  eos_skill_primitive_response_v2 response{sizeof(response), {}, {}};
  if (execution->host->call_primitive(execution->host->context, &request, &response) !=
      EOS_SKILL_HOST_OK)
    return EOS_SKILL_EXECUTE_FAILED;
  const eos_skill_completion_v2 result{sizeof(result), 1, Text("completed"), Text("{}")};
  return complete(context, &result) == EOS_SKILL_HOST_OK ? EOS_SKILL_EXECUTE_OK
                                                         : EOS_SKILL_EXECUTE_FAILED;
}
eos_skill_execute_status_v2 CheckInheritedFd(void*, const eos_skill_execution_v2*,
                                              eos_skill_complete_v2 complete, void* context) {
  if (fcntl(250, F_GETFD) != -1 || errno != EBADF) return EOS_SKILL_EXECUTE_FAILED;
  const eos_skill_completion_v2 result{sizeof(result), 1, Text("completed"), Text("{}")};
  return complete(context, &result) == EOS_SKILL_HOST_OK ? EOS_SKILL_EXECUTE_OK
                                                         : EOS_SKILL_EXECUTE_FAILED;
}
constexpr eos_skill_primitive_reference_v2 kPrimitives[] = {
    {sizeof(eos_skill_primitive_reference_v2), Text("base.posture"), Text("1.0.0")}};
constexpr eos_skill_primitive_reference_v2 kVelocityPrimitives[] = {
    {sizeof(eos_skill_primitive_reference_v2), Text("base.velocity"), Text("1.0.0")}};
constexpr eos_skill_descriptor_v2 kSkills[] = {
    {sizeof(eos_skill_descriptor_v2), Text("fixture.worker_posture"), Text("1.0.0"),
     Text("Worker capability test"),
     Text(R"({"type":"object","properties":{"target":{"type":"object","properties":{"part":{"type":"string"}},"required":["part"]}},"required":["target"]})"),
     Text(R"({"type":"object"})"), 1, kPrimitives, 0, nullptr, 0, nullptr, 0, nullptr,
     1, nullptr, Execute, 0, nullptr},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.worker_velocity"), Text("1.0.0"),
     Text("Worker cancellation test"),
     Text(R"({"type":"object","properties":{"target":{"type":"object","properties":{"part":{"type":"string"}},"required":["part"]}},"required":["target"]})"),
     Text(R"({"type":"object"})"), 1, kVelocityPrimitives, 0, nullptr, 0, nullptr, 0, nullptr,
     1, nullptr, ExecuteVelocity, 0, nullptr},
    {sizeof(eos_skill_descriptor_v2), Text("fixture.worker_fd_check"), Text("1.0.0"),
     Text("Worker inherited descriptor test"), Text(R"({"type":"object"})"),
     Text(R"({"type":"object"})"), 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr,
     0, nullptr, CheckInheritedFd, 0, nullptr}};
constexpr eos_skill_library_v2 kLibrary = {
    EOS_SKILL_ABI_VERSION_V2, sizeof(eos_skill_library_v2), Text("fixture.worker"),
    Text("1.0.0"), Text("test"), 3, kSkills, sizeof(eos_skill_descriptor_v2)};
}  // namespace

extern "C" EOS_SKILL_EXPORT const eos_skill_library_v2* eos_skill_library_query_v2(uint32_t abi) {
  return abi == EOS_SKILL_ABI_VERSION_V2 ? &kLibrary : nullptr;
}
