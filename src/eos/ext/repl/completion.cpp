#include "completion.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace eos::ext::repl {
namespace {

const std::vector<std::string> kCommands = {"?",
                                            "audit-status",
                                            "auto",
                                            "call",
                                            "cancel-motion",
                                            "capture",
                                            "connect",
                                            "describe",
                                            "detect-faces",
                                            "detect-humans",
                                            "disconnect",
                                            "discover",
                                            "estimate-human-pose",
                                            "execution",
                                            "executions",
                                            "exit",
                                            "help",
                                            "history",
                                            "infer-vad",
                                            "journalctl",
                                            "kill",
                                            "leave",
                                            "ls",
                                            "observe-objects",
                                            "perception-roles",
                                            "pkill",
                                            "ps",
                                            "quit",
                                            "read",
                                            "rebuild",
                                            "refresh",
                                            "resources",
                                            "runtime",
                                            "set",
                                            "shutdown",
                                            "sig",
                                            "skill",
                                            "skills",
                                            "start",
                                            "state",
                                            "status",
                                            "watch",
                                            "watch-execution",
                                            "watch-state",
                                            "waypoint",
                                            "waypoints",
                                            "workflow",
                                            "workflows"};

std::vector<std::string> Words(const std::string& input) {
  std::istringstream stream(input);
  std::vector<std::string> words;
  std::string word;
  while (stream >> word) words.push_back(word);
  if (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())) != 0)
    words.emplace_back();
  return words;
}

std::vector<std::string> Matching(const std::vector<std::string>& candidates,
                                  const std::string& prefix) {
  std::vector<std::string> matches;
  for (const auto& candidate : candidates) {
    if (candidate.compare(0, prefix.size(), prefix) == 0) matches.push_back(candidate);
  }
  return matches;
}

std::set<std::string> UsedArguments(const std::vector<std::string>& words,
                                    std::size_t first_argument) {
  std::set<std::string> used;
  for (std::size_t i = first_argument; i + 1 < words.size(); ++i)
    used.insert(words[i].substr(0, words[i].find('=')));
  return used;
}

std::string ParameterSkeleton(const std::map<std::string, ArgumentInfo>& arguments,
                              const std::set<std::string>& used) {
  std::string result;
  for (const auto& [name, unused] : arguments) {
    if (used.count(name) != 0) continue;
    if (!result.empty()) result += ' ';
    result += name + "=";
  }
  return result;
}

std::string SkillParameterSkeleton(const Catalog* catalog,
                                   const std::map<std::string, ArgumentInfo>& arguments,
                                   const std::set<std::string>& used) {
  std::string result;
  for (const auto& [name, info] : arguments) {
    if (used.count(name) != 0) continue;
    if (!result.empty()) result += ' ';
    result += name + "=";
    const auto values = catalog->SkillArgumentValues(name, info);
    if (values.size() == 1) result += values.front();
  }
  return result;
}

std::string Usage(const std::string& command) {
  if (command == "auto") return "auto base [device-id]";
  if (command == "call")
    return "call <primitive> <version> <device|-> [key=value ... | --prompt | --json [OBJECT]]";
  if (command == "capture") return "capture <robot-name> <camera-device-id> [1|2]";
  if (command == "cancel-motion") return "cancel-motion <command-id>";
  if (command == "history")
    return "history <uri> (--sequence N | --before TIMESTAMP | --after TIMESTAMP | "
           "--nearest TIMESTAMP | --range FIRST LAST --limit N)";
  if (command == "infer-vad" || command == "observe-objects" ||
      command == "detect-humans" || command == "estimate-human-pose" ||
      command == "detect-faces")
    return command + " --json [OBJECT]";
  if (command == "read" || command == "watch") return command + " <uri>";
  if (command == "connect") return "connect [host] [port]";
  if (command == "set") return "set full|brief on|off";
  if (command == "help") return "help [command]";
  if (command == "resources") return "resources [list]";
  if (command == "status") return "status [base|arm|gripper [robot-name]]";
  if (command == "waypoints") return "waypoints <robot-name>";
  if (command == "waypoint")
    return "waypoint get|delete <robot-name> <name> | waypoint put <robot-name> <name> "
           "<x-m> <y-m> <yaw-rad> | waypoint reload";
  if (command == "skill") return "skill <name> <version>";
  if (command == "start")
    return "start <name> [version] [--timeout DURATION] [--detach] [arguments]";
  if (command == "execution" || command == "kill") return command + " <execution-id>";
  if (command == "state") return "state <execution-id>";
  if (command == "watch-execution" || command == "journalctl")
    return command + " <execution-id> [after-sequence]";
  if (command == "watch-state") return "watch-state <execution-id> [after-revision]";
  if (command == "pkill") return "pkill <skill> [version]";
  if (command == "ps")
    return "ps [skill [version]] [--status STATUS[,STATUS...]] [--all] [--watch]";
  if (command == "sig") return "sig <signal> <execution-id> [payload]";
  if (command == "workflow")
    return "workflow <id> | workflow watch [id] | workflow apply [--json|--prompt] | "
           "workflow templates|show|run <template> [workflow-id]";
  if (command == "describe") return "describe primitive|skill <name> <version> [device]";
  return {};
}

}  // namespace

bool CompletionEngine::IsCommand(const std::string& word) const {
  return std::find(kCommands.begin(), kCommands.end(), word) != kCommands.end();
}

std::vector<std::string> CompletionEngine::Complete(const std::string& input,
                                                    int* context_length) const {
  std::vector<std::string> words = Words(input);
  if (words.empty()) words.emplace_back();
  const std::string current = words.back();
  *context_length = static_cast<int>(current.size());
  if (words.size() == 1) {
    return Matching(kCommands, current);
  }

  const std::string& command = words[0];
  if (command == "help" && words.size() == 2) return Matching(kCommands, current);
  if (command == "auto") {
    if (words.size() == 2) return Matching({"base"}, current);
    if (words.size() == 3 && words[1] == "base")
      return Matching(catalog_->DevicesForClass("base"), current);
  }
  if (command == "set") {
    if (words.size() == 2) return Matching({"brief", "full"}, current);
    if (words.size() == 3 && (words[1] == "brief" || words[1] == "full"))
      return Matching({"on", "off"}, current);
  }
  if (command == "resources" && words.size() == 2) return Matching({"list"}, current);
  if (command == "status") {
    if (words.size() == 2) return Matching({"arm", "base", "gripper"}, current);
    if (words.size() == 3 && (words[1] == "arm" || words[1] == "base" || words[1] == "gripper"))
      return Matching(catalog_->Robots(), current);
  }
  if (command == "waypoints" && words.size() == 2)
    return Matching(catalog_->Robots(), current);
  if (command == "waypoint") {
    if (words.size() == 2) return Matching({"delete", "get", "put", "reload"}, current);
    if (words.size() == 3 &&
        (words[1] == "delete" || words[1] == "get" || words[1] == "put"))
      return Matching(catalog_->Robots(), current);
    if (words.size() == 4 &&
        (words[1] == "delete" || words[1] == "get" || words[1] == "put"))
      return Matching(catalog_->NavigationPointsFor(words[2]), current);
  }
  if (command == "capture") {
    if (words.size() == 2) return Matching(catalog_->Robots(), current);
    if (words.size() == 3) return Matching(catalog_->DevicesForClass("camera"), current);
    if (words.size() == 4) return Matching({"1", "2"}, current);
  }
  if ((command == "read" || command == "watch" || command == "history") && words.size() == 2) {
    return Matching(catalog_->Resources(), current);
  }
  if (command == "history" && words.size() == 3)
    return Matching({"--after", "--before", "--nearest", "--range", "--sequence"}, current);
  if (command == "history" && words.size() == 6 && words[2] == "--range")
    return Matching({"--limit"}, current);
  if ((command == "infer-vad" || command == "observe-objects" ||
       command == "detect-humans" || command == "estimate-human-pose" ||
       command == "detect-faces") &&
      words.size() == 2)
    return Matching({"--json"}, current);
  if ((command == "execution" || command == "kill" || command == "state" ||
       command == "watch-execution" || command == "watch-state" || command == "journalctl") &&
      words.size() == 2) {
    return Matching(catalog_->ExecutionIds(), current);
  }
  if (command == "sig") {
    if (words.size() == 2) return Matching(catalog_->SignalNames(), current);
    if (words.size() == 3) return Matching(catalog_->ActiveExecutionIdsFor(words[1]), current);
    if (words.size() >= 4 && words[1] != "cancel" && words[1] != "pause" && words[1] != "resume") {
      const std::string declared =
          words[1] == "pause" || words[1] == "resume" ? "skill.command" : words[1];
      auto arguments = catalog_->SignalArgumentsFor(words[2], declared);
      if (words[1] == "pause" || words[1] == "resume") arguments.erase("command");
      if (current.find('=') != std::string::npos) {
        const std::string key = current.substr(0, current.find('='));
        const auto found = arguments.find(key);
        if (found == arguments.end()) return {};
        std::vector<std::string> values;
        for (const auto& value : catalog_->SkillArgumentValues(key, found->second))
          values.push_back(key + "=" + value);
        return Matching(values, current);
      }
      const auto used = UsedArguments(words, 3);
      if (current.empty()) {
        const std::string skeleton = SkillParameterSkeleton(catalog_, arguments, used);
        if (!skeleton.empty()) return {skeleton};
      }
      std::vector<std::string> candidates{"--json", "--prompt"};
      for (const auto& [name, unused] : arguments)
        if (used.count(name) == 0) candidates.push_back(name + "=");
      return Matching(candidates, current);
    }
  }
  if (command == "call") {
    if (words.size() == 2) return Matching(catalog_->Primitives(), current);
    if (words.size() == 3) return Matching(catalog_->PrimitiveVersionsFor(words[1]), current);
    if (words.size() == 4) {
      return Matching(catalog_->DevicesFor(words[1], words[2]), current);
    }
    if (words.size() >= 5) {
      const auto arguments = catalog_->ArgumentsFor(words[1], words[2], words[3]);
      if (current.find('=') != std::string::npos) {
        const std::string key = current.substr(0, current.find('='));
        const auto found = arguments.find(key);
        if (found == arguments.end()) return {};
        std::vector<std::string> values;
        for (const auto& value : found->second.values) values.push_back(key + "=" + value);
        return Matching(values, current);
      }
      const auto used = UsedArguments(words, 4);
      if (current.empty()) {
        const std::string skeleton = ParameterSkeleton(arguments, used);
        if (!skeleton.empty()) return {skeleton};
      }
      std::vector<std::string> candidates{"--json", "--prompt"};
      for (const auto& [name, unused] : arguments) {
        if (used.count(name) == 0) candidates.push_back(name + "=");
      }
      return Matching(candidates, current);
    }
  }
  if (command == "skill" || command == "start") {
    if (words.size() == 2) return Matching(catalog_->Skills(), current);
    const auto versions = catalog_->SkillVersionsFor(words[1]);
    if (command == "skill" && words.size() == 3) return Matching(versions, current);
    const bool explicit_version = words.size() >= 3 && std::find(versions.begin(), versions.end(),
                                                                 words[2]) != versions.end();
    if (command == "start" && versions.size() != 1 && words.size() == 3)
      return Matching(versions, current);
    const std::string version =
        explicit_version ? words[2] : (versions.size() == 1 ? versions.front() : "");
    const std::size_t first_argument = explicit_version ? 3 : 2;
    if (command == "start" && !version.empty() && words.size() > first_argument) {
      const auto arguments = catalog_->SkillArgumentsFor(words[1], version);
      if (current.find('=') != std::string::npos) {
        const std::string key = current.substr(0, current.find('='));
        const auto found = arguments.find(key);
        if (found == arguments.end()) return {};
        std::vector<std::string> values;
        for (const auto& value : catalog_->SkillArgumentValues(key, found->second))
          values.push_back(key + "=" + value);
        return Matching(values, current);
      }
      const auto used = UsedArguments(words, first_argument);
      if (current.empty()) {
        const std::string skeleton = SkillParameterSkeleton(catalog_, arguments, used);
        if (!skeleton.empty()) return {skeleton};
      }
      std::vector<std::string> candidates{"--timeout", "--detach", "--json", "--prompt"};
      for (const auto& [name, unused] : arguments)
        if (used.count(name) == 0) candidates.push_back(name + "=");
      return Matching(candidates, current);
    }
  }
  if (command == "ps") {
    static const std::vector<std::string> statuses = {
        "QUEUED", "RUNNING", "SUCCEEDED", "REJECTED", "CANCELLED", "TIMED_OUT", "FAILED"};
    const auto status = std::find(words.begin() + 1, words.end(), "--status");
    if (status != words.end()) {
      if (status + 1 != words.end()) {
        const std::size_t comma = current.rfind(',');
        if (comma == std::string::npos) return Matching(statuses, current);
        const std::string prefix = current.substr(0, comma + 1);
        std::vector<std::string> values;
        for (const auto& value : Matching(statuses, current.substr(comma + 1)))
          values.push_back(prefix + value);
        return values;
      }
      return statuses;
    }
    if (words.size() == 2) {
      std::vector<std::string> candidates = catalog_->Skills();
      candidates.insert(candidates.end(), {"--all", "--status", "--watch"});
      return Matching(candidates, current);
    }
    if (words.size() == 3) {
      std::vector<std::string> candidates = catalog_->SkillVersionsFor(words[1]);
      candidates.insert(candidates.end(), {"--all", "--status", "--watch"});
      return Matching(candidates, current);
    }
    return Matching({"--all", "--status", "--watch"}, current);
  }
  if (command == "describe" && words.size() == 2) return Matching({"primitive", "skill"}, current);
  if (command == "workflow" && words.size() == 2)
    return Matching({"apply", "run", "show", "templates", "watch"}, current);
  if (command == "workflow" && words.size() == 3 &&
       (words[1] == "show" || words[1] == "run"))
    return Matching(catalog_->WorkflowTemplates(), current);
  if (command == "describe" && words.size() == 3 && words[1] == "primitive")
    return Matching(catalog_->Primitives(), current);
  if (command == "describe" && words.size() == 4 && words[1] == "primitive")
    return Matching(catalog_->PrimitiveVersionsFor(words[2]), current);
  if (command == "describe" && words.size() == 3 && words[1] == "skill")
    return Matching(catalog_->Skills(), current);
  if (command == "describe" && words.size() == 4 && words[1] == "skill")
    return Matching(catalog_->SkillVersionsFor(words[2]), current);
  return {};
}

std::string CompletionEngine::Hint(const std::string& input, int* context_length) const {
  const auto words = Words(input);
  if (words.empty()) return "Tab completes commands; help shows examples";
  if (words[0] == "call" && words.size() >= 5) {
    const std::string key = words.back().substr(0, words.back().find('='));
    const auto arguments = catalog_->ArgumentsFor(words[1], words[2], words[3]);
    const auto found = arguments.find(key);
    if (found != arguments.end()) {
      *context_length = 0;
      const std::string limit = catalog_->LimitHint(words[1], words[2], words[3], key);
      if (!found->second.description.empty() && !limit.empty())
        return found->second.description + " (" + limit + ")";
      return found->second.description.empty() ? limit : found->second.description;
    }
  }
  const std::string usage = Usage(words[0]);
  if (usage.empty()) return {};
  *context_length = 0;
  return usage;
}

}  // namespace eos::ext::repl
