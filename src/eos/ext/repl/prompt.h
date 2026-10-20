/// Prompt identity derived from authoritative gateway discovery.
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace eos::ext::repl {

inline std::string PromptIdentity(const nlohmann::json& discovery) {
  if (!discovery.is_object() || !discovery.contains("robot") ||
      !discovery.at("robot").is_object()) {
    return "offline";
  }
  const nlohmann::json& robot = discovery.at("robot");
  if (!robot.contains("name") || !robot.at("name").is_string()) return "offline";
  const std::string name = robot.at("name").get<std::string>();
  return name.empty() ? "offline" : name;
}

}  // namespace eos::ext::repl
