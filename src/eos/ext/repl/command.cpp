/// Northbound REPL input parsing — see command.h. Pure string/JSON work;
/// the wire never enters here so tests need no sockets.

#include "command.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <utility>
#include <vector>

#include "src/eos/lib/uuid.h"

namespace eos::ext::repl {
namespace {

using json = nlohmann::json;

/// Shell-like tokenization for handwritten commands. Single/double quotes and
/// backslash escapes keep whitespace inside one token; the parser records
/// whether a value was quoted so a quoted numeric-looking value stays text.
struct Tokens {
  std::vector<std::string> values;
  std::vector<bool> quoted;
  std::vector<std::size_t> ends;
  std::string error;
};

Tokens Tokenize(const std::string& line) {
  Tokens result;
  std::string token;
  char quote = 0;
  bool escaped = false;
  bool token_quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (escaped) {
      token += c;
      escaped = false;
    } else if (c == '\\' && quote != '\'') {
      escaped = true;
    } else if ((c == '"' || c == '\'') && (quote == 0 || quote == c)) {
      token_quoted = true;
      quote = quote == 0 ? c : 0;
    } else if (std::isspace(static_cast<unsigned char>(c)) != 0 && quote == 0) {
      if (!token.empty()) {
        result.values.push_back(std::move(token));
        result.quoted.push_back(token_quoted);
        result.ends.push_back(i);
        token.clear();
        token_quoted = false;
      }
    } else {
      token += c;
    }
  }
  if (escaped) {
    result.error = "trailing escape character";
    return result;
  }
  if (quote != 0) {
    result.error = "unterminated quote";
    return result;
  }
  if (!token.empty()) {
    result.values.push_back(std::move(token));
    result.quoted.push_back(token_quoted);
    result.ends.push_back(line.size());
  }
  return result;
}

/// Coerces one `k=v` value to JSON: double-quoted → string, `true`/`false`
/// → bool, integer → number, decimal → number, anything else → string. The
/// primitive schemas server-side are the authority; this only shapes obvious
/// scalars so `valid_for_ms=250` arrives as a number.
json CoerceValue(const std::string& raw, bool quoted) {
  if (quoted) return raw;
  if (!raw.empty() && (raw.front() == '[' || raw.front() == '{')) {
    try {
      return json::parse(raw);
    } catch (const json::parse_error&) {
      return raw;
    }
  }
  if (raw == "null") return nullptr;
  if (raw == "true") {
    return true;
  }
  if (raw == "false") {
    return false;
  }
  const char* begin = raw.c_str();
  char* end = nullptr;
  const long long integer = std::strtoll(begin, &end, 10);
  if (end != nullptr && *end == '\0' && !raw.empty() &&
      raw.find_first_of(".eE") == std::string::npos) {
    return integer;
  }
  const double real = std::strtod(begin, &end);
  if (end != nullptr && *end == '\0' && !raw.empty()) {
    return real;
  }
  return raw;
}

/// Builds `arguments` from `k=v` tokens (token[0] onward of a `call`).
/// Returns nullopt-shaped via `error` on a token without `=`.
bool BuildArguments(const Tokens& tokens, std::size_t from, json* arguments, std::string* error) {
  for (std::size_t i = from; i < tokens.values.size(); ++i) {
    const std::size_t eq = tokens.values[i].find('=');
    if (eq == std::string::npos || eq == 0) {
      *error = "expected key=value, got '" + tokens.values[i] + "'";
      return false;
    }
    const std::string key = tokens.values[i].substr(0, eq);
    if (arguments->contains(key)) {
      *error = "duplicate argument '" + key + "'";
      return false;
    }
    const std::string raw = tokens.values[i].substr(eq + 1);
    json value = CoerceValue(raw, tokens.quoted[i]);
    if (!tokens.quoted[i] && !raw.empty() && (raw.front() == '[' || raw.front() == '{') &&
        value.is_string()) {
      *error = "malformed JSON value for '" + key + "'";
      return false;
    }
    (*arguments)[key] = std::move(value);
  }
  return true;
}

/// A friendly request action with method and params filled in.
Action Request(const std::string& method, json params) {
  Action action;
  action.kind = Action::Kind::kRequest;
  action.method = method;
  action.params = std::move(params);
  return action;
}

/// A kind-only action (help, quit, refresh, or disconnect).
Action Simple(Action::Kind kind) {
  Action action;
  action.kind = kind;
  return action;
}

/// The parse-failure action.
Action Error(std::string message) {
  Action action;
  action.kind = Action::Kind::kError;
  action.error = std::move(message);
  return action;
}

bool ParseUnsigned(const std::string& raw, std::uint64_t* value) {
  if (raw.empty() || raw.front() == '-') return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(raw.c_str(), &end, 10);
  if (errno == ERANGE || end == raw.c_str() || *end != '\0') return false;
  *value = static_cast<std::uint64_t>(parsed);
  return true;
}

Action JsonRequest(const std::string& method, const std::string& command, const Tokens& tokens,
                   const std::string& trimmed) {
  if (tokens.values.size() < 2 || tokens.values[1] != "--json") {
    return Error("usage: " + command + " --json [{...}]");
  }
  Action action = Request(method, json::object());
  action.history_prefix = command;
  if (tokens.values.size() == 2) {
    action.prompt_json = true;
    return action;
  }
  const std::size_t first = trimmed.find_first_not_of(" \t", tokens.ends[1]);
  try {
    action.params = json::parse(trimmed.substr(first));
  } catch (const std::exception& exception) {
    return Error("invalid JSON: " + std::string(exception.what()));
  }
  if (!action.params.is_object()) return Error("request JSON must be an object");
  return action;
}

/// A URI-taking command; usage line names the verb for the error message.
Action UriPrimitive(Action::Kind kind, const std::vector<std::string>& tokens, const char* verb) {
  if (tokens.size() != 2) {
    return Error(std::string("usage: ") + verb + " <uri>");
  }
  Action action;
  action.kind = kind;
  action.uri = tokens[1];
  return action;
}

}  // namespace

Action CommandParser::Parse(const std::string& line) {
  const std::size_t start = line.find_first_not_of(" \t");
  if (start == std::string::npos || line[start] == '#') {
    return Action{};  // kNone
  }
  const std::string trimmed = line.substr(start);
  const Tokens parsed = Tokenize(trimmed);
  if (!parsed.error.empty()) return Error(parsed.error);
  const std::vector<std::string>& tokens = parsed.values;
  if (tokens.empty()) return Action{};
  const std::string& head = tokens[0];
  if (head == "help" || head == "?") {
    if (tokens.size() > 2) return Error("usage: help [command]");
    Action action = Simple(Action::Kind::kHelp);
    if (tokens.size() == 2) action.help_topic = tokens[1];
    return action;
  }
  if (head == "quit" || head == "exit") {
    if (tokens.size() != 1) return Error("usage: " + head);
    return Simple(Action::Kind::kQuit);
  }
  if (head == "leave") {
    if (tokens.size() != 1) return Error("usage: leave");
    return Simple(Action::Kind::kLeave);
  }
  if (head == "shutdown") {
    if (tokens.size() != 1) return Error("usage: shutdown");
    return Simple(Action::Kind::kShutdown);
  }
  if (head == "rebuild") {
    if (tokens.size() != 1) return Error("usage: rebuild");
    return Simple(Action::Kind::kRebuild);
  }
  if (head == "connect") {
    Action action;
    action.kind = Action::Kind::kConnect;
    if (tokens.size() > 1) {
      action.host = tokens[1];
    }
    if (tokens.size() > 2) {
      char* end = nullptr;
      const long port = std::strtol(tokens[2].c_str(), &end, 10);
      if (end == tokens[2].c_str() || *end != '\0' || port <= 0 || port > 65535) {
        return Error("port must be an integer in (0,65535]");
      }
      action.port = static_cast<std::uint16_t>(port);
    }
    if (tokens.size() > 3) {
      return Error("usage: connect [host] [port]");
    }
    return action;
  }
  if (head == "disconnect") {
    if (tokens.size() != 1) return Error("usage: disconnect");
    return Simple(Action::Kind::kDisconnect);
  }
  if (head == "refresh") {
    if (tokens.size() != 1) return Error("usage: refresh");
    return Simple(Action::Kind::kRefresh);
  }
  if (head == "auto") {
    if (tokens.size() < 2 || tokens.size() > 3 || tokens[1] != "base")
      return Error("usage: auto base [device-id]");
    Action action = Simple(Action::Kind::kAutoBase);
    action.params = {{"device_id", tokens.size() == 3 ? tokens[2] : "base-0"}};
    return action;
  }
  if (head == "status") {
    if (tokens.size() == 1) return Request("GetStatus", nullptr);
    if (tokens.size() < 2 || tokens.size() > 3 ||
        (tokens[1] != "base" && tokens[1] != "arm" && tokens[1] != "gripper"))
      return Error("usage: status [base|arm|gripper [robot-name]]");
    return Request(
        "ReadStatusResource",
        {{"class", tokens[1]}, {"robot_name", tokens.size() == 3 ? tokens[2] : std::string()}});
  }
  if (head == "audit-status") {
    if (tokens.size() != 1) return Error("usage: audit-status");
    return Request("GetAuditStatus", nullptr);
  }
  if (head == "waypoints") {
    if (tokens.size() != 2) return Error("usage: waypoints <robot-name>");
    return Request("ListNavigationPoints", {{"robot_name", tokens[1]}});
  }
  if (head == "waypoint") {
    constexpr const char* kUsage =
        "usage: waypoint get <robot-name> <name> | waypoint put <robot-name> <name> "
        "<x-m> <y-m> <yaw-rad> | waypoint delete <robot-name> <name> | waypoint reload";
    if (tokens.size() == 2 && tokens[1] == "reload")
      return Request("ReloadNavigationPoints", json::object());
    if (tokens.size() == 4 && (tokens[1] == "get" || tokens[1] == "delete")) {
      return Request(tokens[1] == "get" ? "GetNavigationPoint" : "DeleteNavigationPoint",
                     {{"robot_name", tokens[2]}, {"name", tokens[3]}});
    }
    if (tokens.size() == 7 && tokens[1] == "put") {
      json x = CoerceValue(tokens[4], false);
      json y = CoerceValue(tokens[5], false);
      json yaw = CoerceValue(tokens[6], false);
      if (!x.is_number() || !y.is_number() || !yaw.is_number()) return Error(kUsage);
      return Request("PutNavigationPoint", {{"robot_name", tokens[2]},
                                            {"name", tokens[3]},
                                            {"x_m", x},
                                            {"y_m", y},
                                            {"yaw_rad", yaw}});
    }
    return Error(kUsage);
  }
  if (head == "skills") {
    if (tokens.size() != 1) return Error("usage: skills");
    return Request("ListSkills", nullptr);
  }
  if (head == "skill") {
    if (tokens.size() != 3) return Error("usage: skill <name> <version>");
    return Request("GetSkill", {{"reference", {{"name", tokens[1]}, {"version", tokens[2]}}}});
  }
  if (head == "executions") {
    if (tokens.size() != 1) return Error("usage: executions");
    return Request("ListExecutions", nullptr);
  }
  if (head == "execution") {
    if (tokens.size() != 2) return Error("usage: execution <execution-id>");
    return Request("GetExecution", {{"execution_id", tokens[1]}});
  }
  if (head == "state") {
    if (tokens.size() != 2) return Error("usage: state <execution-id>");
    return Request("GetExecutionState", {{"execution_id", tokens[1]}});
  }
  if (head == "kill") {
    if (tokens.size() != 2) return Error("usage: kill <execution-id>");
    return Request("TerminateExecution", {{"execution_id", tokens[1]}});
  }
  if (head == "pkill") {
    if (tokens.size() < 2 || tokens.size() > 3) return Error("usage: pkill <skill> [version]");
    json reference = {{"name", tokens[1]}};
    if (tokens.size() == 3) reference["version"] = tokens[2];
    return Request("TerminateExecutions", {{"reference", std::move(reference)},
                                           {"request_id", "repl-" + eos::lib::MintUuidV7()}});
  }
  if (head == "watch-execution" || head == "journalctl") {
    if (tokens.size() < 2 || tokens.size() > 3)
      return Error("usage: " + head + " <execution-id> [after-sequence]");
    Action action;
    action.kind = Action::Kind::kWatchStream;
    action.stream_key = head + ":" + tokens[1];
    action.stream_path = "/v1/eos/executions/" + tokens[1] +
                         (head == "watch-execution" ? ":watch" : "/journal:follow");
    if (tokens.size() == 3)
      action.stream_path +=
          std::string("?") +
          (head == "watch-execution" ? "afterSequence=" : "afterJournalSequence=") + tokens[2];
    return action;
  }
  if (head == "watch-state") {
    if (tokens.size() < 2 || tokens.size() > 3)
      return Error("usage: watch-state <execution-id> [after-revision]");
    Action action;
    action.kind = Action::Kind::kWatchStream;
    action.stream_key = "watch-state:" + tokens[1];
    action.stream_path = "/v1/eos/executions/" + tokens[1] + "/state:watch";
    if (tokens.size() == 3) action.stream_path += "?afterRevision=" + tokens[2];
    return action;
  }
  if (head == "ps") {
    json params = {{"statuses", json::array({"QUEUED", "RUNNING"})}};
    bool all = false;
    bool watch = false;
    bool status_explicit = false;
    std::size_t index = 1;
    while (index < tokens.size() && (tokens[index] == "--all" || tokens[index] == "--watch")) {
      all = all || tokens[index] == "--all";
      watch = watch || tokens[index] == "--watch";
      ++index;
    }
    if (index < tokens.size() && tokens[index] != "--status" && tokens[index] != "--all" &&
        tokens[index] != "--watch") {
      params["skill_name"] = tokens[index++];
      if (index < tokens.size() && tokens[index] != "--status" && tokens[index] != "--all" &&
          tokens[index] != "--watch")
        params["skill_version"] = tokens[index++];
    }
    while (index < tokens.size()) {
      if (tokens[index] == "--all") {
        all = true;
        ++index;
        continue;
      }
      if (tokens[index] == "--watch") {
        watch = true;
        ++index;
        continue;
      }
      if (tokens[index++] != "--status" || index == tokens.size())
        return Error("usage: ps [skill [version]] [--status STATUS[,STATUS...]] [--all] [--watch]");
      params["statuses"] = json::array();
      status_explicit = true;
      while (index < tokens.size()) {
        if (tokens[index] == "--all" || tokens[index] == "--watch") break;
        std::size_t begin = 0;
        while (begin <= tokens[index].size()) {
          const std::size_t comma = tokens[index].find(',', begin);
          const std::string status = tokens[index].substr(begin, comma - begin);
          if (status.empty()) return Error("ps status must not be empty");
          params["statuses"].push_back(status);
          if (comma == std::string::npos) break;
          begin = comma + 1;
        }
        ++index;
      }
      if (params["statuses"].empty()) return Error("ps status must not be empty");
    }
    if (all && !status_explicit) params["statuses"] = json::array();
    if (!watch) return Request("PsExecutions", std::move(params));
    Action action;
    action.kind = Action::Kind::kWatchStream;
    action.stream_key = "ps";
    action.stream_path = "/v1/eos/executions:monitor";
    std::vector<std::string> query;
    if (params.contains("skill_name"))
      query.push_back("skillName=" + params["skill_name"].get<std::string>());
    if (params.contains("skill_version"))
      query.push_back("skillVersion=" + params["skill_version"].get<std::string>());
    std::string statuses;
    for (const auto& status : params["statuses"])
      statuses += (statuses.empty() ? "" : ",") + status.get<std::string>();
    if (!statuses.empty()) query.push_back("status=" + statuses);
    if (!query.empty()) {
      action.stream_path += "?";
      for (std::size_t i = 0; i < query.size(); ++i)
        action.stream_path += (i == 0 ? "" : "&") + query[i];
    }
    return action;
  }
  if (head == "workflows") {
    if (tokens.size() != 1) return Error("usage: workflows");
    return Request("ListActionLists", nullptr);
  }
  if (head == "workflow") {
    if ((tokens.size() == 2 || tokens.size() == 3) && tokens[1] == "watch") {
      Action action = Simple(Action::Kind::kWorkflowWatch);
      if (tokens.size() == 3) action.uri = tokens[2];
      return action;
    }
    if (tokens.size() == 2 && tokens[1] == "templates") {
      return Request("ListWorkflowTemplates", nullptr);
    }
    if (tokens.size() == 3 && tokens[1] == "show") {
      return Request("GetWorkflowTemplate", {{"name", tokens[2]}});
    }
    if ((tokens.size() == 3 || tokens.size() == 4) && tokens[1] == "run") {
      json params = {{"name", tokens[2]}};
      if (tokens.size() == 4) params["workflow_id"] = tokens[3];
      return Request("RunWorkflowTemplate", std::move(params));
    }
    if (tokens.size() >= 2 && (tokens[1] == "show" || tokens[1] == "run"))
      return Error("usage: workflow " + tokens[1] + " <template>" +
                   (tokens[1] == "run" ? " [workflow-id]" : ""));
    if (tokens.size() == 2 && tokens[1] != "apply")
      return Request("GetActionList", {{"workflow_id", tokens[1]}});
    if (tokens.size() >= 2 && tokens[1] == "apply") {
      Action action = Request("ApplyActionList", json::object());
      action.prompt_json = true;
      action.guided_prompt = true;
      action.history_prefix = "workflow apply";
      if (tokens.size() > 2 && tokens[2] == "--json") {
        action.guided_prompt = false;
        if (tokens.size() == 3) return action;
        const std::size_t json_start = parsed.ends[2];
        const std::size_t first = trimmed.find_first_not_of(" \t", json_start);
        try {
          action.params = json::parse(trimmed.substr(first));
        } catch (const std::exception& exception) {
          return Error("invalid JSON: " + std::string(exception.what()));
        }
        action.prompt_json = false;
        action.guided_prompt = false;
        return action;
      }
      if (tokens.size() == 3 && tokens[2] == "--prompt") return action;
      if (tokens.size() != 2) return Error("usage: workflow apply [--prompt|--json [{...}]]");
      return action;
    }
    return Error("usage: workflow <workflow-id> | workflow watch [workflow-id] | workflow apply "
                  "[--prompt|--json] | workflow templates|show|run <template> [workflow-id]");
  }
  if (head == "runtime") {
    if (tokens.size() != 1) return Error("usage: runtime");
    return Request("GetRuntimeSnapshot", nullptr);
  }
  if (head == "describe") {
    if (tokens.size() < 4 || tokens.size() > 5 ||
        (tokens[1] != "primitive" && tokens[1] != "skill"))
      return Error(
          "usage: describe primitive <name> <version> [device] | describe skill <name> <version>");
    if (tokens[1] == "skill") {
      if (tokens.size() != 4) return Error("usage: describe skill <name> <version>");
      Action action;
      action.kind = Action::Kind::kDescribe;
      action.describe_kind = "skill";
      action.params = {{"name", tokens[2]}, {"version", tokens[3]}};
      return action;
    }
    Action action;
    action.kind = Action::Kind::kDescribe;
    action.describe_kind = "primitive";
    action.params = {{"name", tokens[2]}, {"version", tokens[3]}};
    if (tokens.size() == 5) action.params["device_id"] = tokens[4];
    return action;
  }
  if (head == "capture") {
    if (tokens.size() < 3 || tokens.size() > 4) {
      return Error("usage: capture <robot-name> <camera-device-id> [1|2]");
    }
    long count = 1;
    if (tokens.size() == 4) {
      char* end = nullptr;
      count = std::strtol(tokens[3].c_str(), &end, 10);
      if (end == tokens[3].c_str() || *end != '\0' || count < 1 || count > 2) {
        return Error("frame count must be 1 or 2");
      }
    }
    return Request("CaptureFrames",
                   {{"robot_name", tokens[1]}, {"device_id", tokens[2]}, {"frame_count", count}});
  }
  if (head == "cancel-motion") {
    if (tokens.size() != 2) return Error("usage: cancel-motion <command-id>");
    return Request("CancelPrimitive", {{"command_id", tokens[1]}});
  }
  if (head == "infer-vad") return JsonRequest("InferVad", head, parsed, trimmed);
  if (head == "observe-objects") return JsonRequest("ObserveObjects", head, parsed, trimmed);
  if (head == "detect-humans") return JsonRequest("DetectHumans", head, parsed, trimmed);
  if (head == "estimate-human-pose") return JsonRequest("EstimateHumanPose", head, parsed, trimmed);
  if (head == "detect-faces") return JsonRequest("DetectFaces", head, parsed, trimmed);
  if (head == "perception-roles") {
    if (tokens.size() != 1) return Error("usage: perception-roles");
    return Request("GetPerceptionRoles", nullptr);
  }
  if (head == "discover") {
    if (tokens.size() != 1) {
      return Error("usage: discover");
    }
    return Request("Discover", nullptr);
  }
  if (head == "resources" || head == "ls") {
    if (tokens.size() == 2 && tokens[1] == "list") {
      return Request("ListResources", nullptr);
    }
    if (tokens.size() != 1) {
      return Error("usage: resources");
    }
    return Request("ListResources", nullptr);
  }
  if (head == "read") {
    const Action action = UriPrimitive(Action::Kind::kRequest, tokens, "read");
    if (action.kind != Action::Kind::kError) {
      return Request("ReadResource", json{{"uri", action.uri}});
    }
    return action;
  }
  if (head == "history") {
    constexpr const char* kUsage =
        "usage: history <uri> (--sequence N | --before TIMESTAMP | --after TIMESTAMP | "
        "--nearest TIMESTAMP | --range FIRST LAST --limit N)";
    if (tokens.size() < 4) return Error(kUsage);
    json params{{"uri", tokens[1]}};
    const std::string& selector = tokens[2];
    if (selector == "--sequence") {
      std::uint64_t sequence = 0;
      if (tokens.size() != 4 || !ParseUnsigned(tokens[3], &sequence) || sequence == 0)
        return Error("history sequence must be a non-zero integer");
      params["sequence"] = std::to_string(sequence);
    } else if (selector == "--before" || selector == "--after" || selector == "--nearest") {
      if (tokens.size() != 4) return Error(kUsage);
      params[selector.substr(2)] = tokens[3];
    } else if (selector == "--range") {
      std::uint64_t first = 0;
      std::uint64_t last = 0;
      std::uint64_t limit = 0;
      if (tokens.size() != 7 || tokens[5] != "--limit" || !ParseUnsigned(tokens[3], &first) ||
          !ParseUnsigned(tokens[4], &last) || !ParseUnsigned(tokens[6], &limit))
        return Error(kUsage);
      if (first == 0 || last == 0 || first > last)
        return Error("history range must be non-zero and FIRST must not exceed LAST");
      if (limit == 0 || limit > 1000) return Error("history limit must be in [1, 1000]");
      params["sequenceRange"] = {{"firstSequence", std::to_string(first)},
                                 {"lastSequence", std::to_string(last)},
                                 {"limit", limit}};
    } else {
      return Error(kUsage);
    }
    return Request("QueryResourceHistory", std::move(params));
  }
  if (head == "watch") {
    return UriPrimitive(Action::Kind::kWatch, tokens, "watch");
  }
  if (head == "set") {
    if (tokens.size() == 3 && tokens[1] == "full" && (tokens[2] == "on" || tokens[2] == "off")) {
      Action action;
      action.kind = Action::Kind::kSetFull;
      action.full = tokens[2] == "on";
      return action;
    }
    if (tokens.size() == 3 && tokens[1] == "brief" && (tokens[2] == "on" || tokens[2] == "off")) {
      Action action;
      action.kind = Action::Kind::kSetBrief;
      action.brief = tokens[2] == "on";
      return action;
    }
    return Error("usage: set full|brief on|off");
  }
  if (head == "call") {
    if (tokens.size() < 4) {
      return Error("usage: call <primitive> <version> <device-id|-> [key=value ...]");
    }
    json arguments = json::object();
    bool prompt_json = false;
    bool guided_prompt = false;
    if (tokens.size() > 4 && (tokens[4] == "--json" || tokens[4] == "--prompt")) {
      if (tokens[4] == "--prompt") {
        if (tokens.size() != 5) return Error("--prompt takes no value");
        prompt_json = true;
        guided_prompt = true;
      } else if (tokens.size() > 5) {
        const std::size_t json_start = parsed.ends[4];
        const std::size_t first = trimmed.find_first_not_of(" \t", json_start);
        try {
          arguments = json::parse(trimmed.substr(first));
        } catch (const std::exception& exception) {
          return Error("invalid JSON: " + std::string(exception.what()));
        }
        if (!arguments.is_object()) return Error("JSON arguments must be an object");
      } else {
        prompt_json = true;
      }
    } else if (tokens.size() > 4) {
      std::string error;
      if (!BuildArguments(parsed, 4, &arguments, &error)) {
        return Error(error);
      }
    }
    arguments.erase("command_id");
    const std::string command_id = "repl-" + eos::lib::MintUuidV7();
    Action action = Request("CallPrimitive", json{{"name", tokens[1]},
                                                  {"version", tokens[2]},
                                                  {"device_id", tokens[3] == "-" ? "" : tokens[3]},
                                                  {"command_id", command_id},
                                                  {"arguments", std::move(arguments)}});
    action.prompt_json = prompt_json;
    action.guided_prompt = guided_prompt;
    action.history_prefix = "call " + tokens[1] + " " + tokens[2] + " " + tokens[3];
    return action;
  }
  if (head == "start") {
    if (tokens.size() < 2)
      return Error(
          "usage: start <skill> [version] [--timeout DURATION] [--detach] "
          "[--prompt|--json [{...}]|key=value "
          "...]");
    std::string version;
    std::size_t cursor = 2;
    if (cursor < tokens.size() && tokens[cursor].rfind("--", 0) != 0 &&
        tokens[cursor].find('=') == std::string::npos) {
      version = tokens[cursor++];
    }
    std::string timeout;
    bool detach = false;
    while (cursor < tokens.size() &&
           (tokens[cursor] == "--timeout" || tokens[cursor] == "--detach")) {
      if (tokens[cursor] == "--detach") {
        detach = true;
        ++cursor;
      } else {
        if (cursor + 1 >= tokens.size()) return Error("--timeout requires a duration");
        timeout = tokens[cursor + 1];
        cursor += 2;
      }
    }
    json arguments = json::object();
    bool prompt = false;
    bool guided_prompt = false;
    if (cursor < tokens.size() && (tokens[cursor] == "--prompt" || tokens[cursor] == "--json")) {
      const bool json_mode = tokens[cursor] == "--json";
      if (cursor + 1 < tokens.size()) {
        if (!json_mode) return Error("--prompt takes no value");
        const std::size_t first = trimmed.find_first_not_of(" \t", parsed.ends[cursor]);
        try {
          arguments = json::parse(trimmed.substr(first));
        } catch (const std::exception& exception) {
          return Error("invalid JSON: " + std::string(exception.what()));
        }
        if (!arguments.is_object()) return Error("skill arguments must be an object");
      } else {
        prompt = true;
        guided_prompt = !json_mode;
      }
    } else if (cursor < tokens.size()) {
      std::string error;
      if (!BuildArguments(parsed, cursor, &arguments, &error)) return Error(error);
    }
    json params = {{"reference", {{"name", tokens[1]}, {"version", version}}},
                   {"arguments", std::move(arguments)},
                   {"request_id", "repl-" + eos::lib::MintUuidV7()}};
    if (!timeout.empty()) params["timeout"] = timeout;
    Action action = Request("StartExecution", std::move(params));
    action.prompt_json = prompt;
    action.guided_prompt = guided_prompt;
    action.strict_json =
        cursor < tokens.size() && tokens[cursor] == "--json" && cursor + 1 < tokens.size();
    action.history_prefix = "start " + tokens[1] + (version.empty() ? "" : " " + version);
    action.attach_execution = !detach;
    return action;
  }
  if (head == "sig") {
    if (tokens.size() < 3)
      return Error("usage: sig <signal> <execution-id> [--prompt|--json [{...}]|key=value ...]");
    const std::string& signal = tokens[1];
    const std::string& execution_id = tokens[2];
    if (signal == "cancel") {
      if (tokens.size() != 3) return Error("sig cancel takes no payload");
      return Request("TerminateExecution", {{"execution_id", execution_id}});
    }
    if ((signal == "pause" || signal == "resume") && tokens.size() != 3)
      return Error("sig " + signal + " takes no payload");
    json payload = json::object();
    bool prompt = false;
    bool guided_prompt = false;
    if (tokens.size() > 3 && (tokens[3] == "--prompt" || tokens[3] == "--json")) {
      if (tokens.size() > 4) {
        if (tokens[3] == "--prompt") return Error("--prompt takes no value");
        const std::size_t first = trimmed.find_first_not_of(" \t", parsed.ends[3]);
        try {
          payload = json::parse(trimmed.substr(first));
        } catch (const std::exception& exception) {
          return Error("invalid JSON: " + std::string(exception.what()));
        }
        if (!payload.is_object()) return Error("signal payload must be an object");
      } else {
        prompt = true;
        guided_prompt = tokens[3] == "--prompt";
      }
    } else if (tokens.size() > 3) {
      std::string error;
      if (!BuildArguments(parsed, 3, &payload, &error)) return Error(error);
    }
    if (signal == "pause" || signal == "resume") payload["command"] = signal;
    Action action =
        Request("SignalExecution",
                {{"execution_id", execution_id},
                 {"request_id", "repl-" + eos::lib::MintUuidV7()},
                 {"name", signal == "pause" || signal == "resume" ? "skill.command" : signal},
                 {"payload", std::move(payload)}});
    action.prompt_json = prompt;
    action.guided_prompt = guided_prompt;
    action.history_prefix = "sig " + signal + " " + execution_id;
    return action;
  }
  return Error("unknown command '" + head + "' — try: help");
}

}  // namespace eos::ext::repl
