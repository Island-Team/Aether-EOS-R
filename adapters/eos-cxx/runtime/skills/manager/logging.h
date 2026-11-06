/// Structured, low-noise logging shared by every skill execution profile.
#pragma once

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "spdlog/spdlog.h"

namespace eos::runtime::skills {

/// Shared routed logger. Profile-specific logger names are intentionally
/// avoided so one execution remains searchable in runtime/skills.log.
std::shared_ptr<spdlog::logger>& SkillLogger();

/// Emits one sanitized structured skill event through eos::lib logging.
void LogSkillEvent(spdlog::level::level_enum level, const std::string& event,
                   const nlohmann::json& fields = nlohmann::json::object());

/// Adds stable execution identity fields and emits one lifecycle event.
void LogExecutionEvent(spdlog::level::level_enum level, const std::string& event,
                       const std::string& execution_id, const std::string& skill_name,
                       const std::string& skill_version, const std::string& request_id,
                       const std::string& status, const std::string& reason_code,
                       std::uint64_t sequence, const std::string& message = {},
                       const nlohmann::json& details = nlohmann::json::object());

}  // namespace eos::runtime::skills
