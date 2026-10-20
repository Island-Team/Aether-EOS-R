#include "src/eos/runtime/skills/manager/logging.h"

#include "src/eos/lib/log.h"

namespace eos::runtime::skills {

std::shared_ptr<spdlog::logger>& SkillLogger() {
  static auto logger = eos::lib::GetLogger("skills");
  return logger;
}

void LogSkillEvent(spdlog::level::level_enum level, const std::string& event,
                   const nlohmann::json& fields) {
  eos::lib::LogEvent(SkillLogger(), level, event, fields);
}

void LogExecutionEvent(spdlog::level::level_enum level, const std::string& event,
                       const std::string& execution_id, const std::string& skill_name,
                       const std::string& skill_version, const std::string& request_id,
                       const std::string& status, const std::string& reason_code,
                       std::uint64_t sequence, const std::string& message,
                       const nlohmann::json& details) {
  nlohmann::json fields{
      {"execution_id", execution_id}, {"skill", skill_name}, {"version", skill_version},
      {"request_id", request_id},     {"status", status},    {"reason", reason_code},
      {"sequence", sequence}};
  if (!message.empty()) fields["message"] = message;
  if (details.is_object()) fields.update(details);
  LogSkillEvent(level, event, fields);
}

}  // namespace eos::runtime::skills
