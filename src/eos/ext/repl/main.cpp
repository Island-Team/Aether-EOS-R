/// eos-repl — interactive HTTP/SSE client for the north-bound adapter.
/// Friendly commands map onto the documented service methods. This is a wire
/// client with no runtime or domain-library coupling; eos_lib supplies logging.

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include "catalog.h"
#include "auto_base.h"
#include "command.h"
#include "completion.h"
#include "format.h"
#include "json_input.h"
#include "options.h"
#include "prompt.h"
#include "session.h"
#include "src/eos/lib/log.h"
#include "src/eos/lib/uuid.h"
#include "terminal.h"

namespace {

using json = nlohmann::json;
namespace repl_ns = eos::ext::repl;
constexpr int kRebuildRequested = 75;
constexpr int kQuitRequested = 76;
constexpr int kLeaveRequested = 77;

bool ColorsEnabled() { return ::isatty(STDOUT_FILENO) != 0 && std::getenv("NO_COLOR") == nullptr; }

std::string UrlEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : value) {
    if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
      result.push_back(static_cast<char>(c));
    } else {
      result.push_back('%');
      result.push_back(kHex[c >> 4]);
      result.push_back(kHex[c & 0x0f]);
    }
  }
  return result;
}

bool CursorSequence(const json& value, std::string* sequence) {
  if (value.is_string()) {
    *sequence = value.get<std::string>();
    return !sequence->empty();
  }
  if (value.is_number_unsigned()) {
    *sequence = std::to_string(value.get<std::uint64_t>());
    return true;
  }
  if (value.is_number_integer()) {
    const std::int64_t number = value.get<std::int64_t>();
    if (number < 0) return false;
    *sequence = std::to_string(number);
    return true;
  }
  return false;
}

using repl_ns::Args;

std::filesystem::path ReplLogRoot() {
  if (const char* value = std::getenv("ISLAND_LOG"); value != nullptr && *value != '\0') {
    const std::filesystem::path root(value);
    if (!root.is_absolute()) throw std::runtime_error("ISLAND_LOG must be absolute");
    return root / "eos";
  }
  if (const char* mode = std::getenv("ISLAND_MODE");
      mode != nullptr && std::string(mode) == "system")
    return "/var/log/island/eos";
  if (const char* value = std::getenv("ISLAND_PREFIX"); value != nullptr && *value != '\0') {
    const std::filesystem::path prefix(value);
    if (!prefix.is_absolute()) throw std::runtime_error("ISLAND_PREFIX must be absolute");
    return prefix / "var/log/eos";
  }
  return eos::lib::DefaultLogDirectory();
}

std::string ResolveContracts(const Args& args) {
  if (!args.contracts.empty()) return args.contracts;
  if (const char* value = std::getenv("EOS_CONTRACTS"); value != nullptr && *value != '\0')
    return value;
  if (const char* value = std::getenv("ISLAND_SHARE"); value != nullptr && *value != '\0')
    return value;
  return {};
}

/// The running REPL: prompt loop on the main thread, pushes arriving on
/// the session's io thread. One print mutex keeps the two from tangling;
/// every async print re-issues the prompt so pushed lines stay readable.
class App final {
 public:
  explicit App(Args args)
      : args_(std::move(args)),
        parser_(),
        display_{false, ColorsEnabled(), false},
        session_(),
        catalog_(),
        completion_(&catalog_),
        terminal_(&completion_, ColorsEnabled(), args_.history) {}

  int Run() {
    Print("eos-repl — HTTP/SSE northbound client; 'help' lists commands");
    std::string warning;
    if (!catalog_.LoadContracts(ResolveContracts(args_), &warning) && !warning.empty()) {
      eos::lib::LogEvent(eos::lib::GetLogger("eos-repl"), spdlog::level::warn,
                         "repl_contract_catalog_load_failed", {{"reason", "catalog_unavailable"}});
      Print("[completion] " + warning);
    }
    Connect(args_.host, args_.port, "");
    std::string line;
    while (true) {
      const auto status = terminal_.Read(PromptText(), &line);
      if (status == repl_ns::Terminal::ReadStatus::kInterrupted) continue;
      if (status == repl_ns::Terminal::ReadStatus::kEof) break;
      if (HandleLine(line)) break;
    }
    session_.Close();
    return exit_code_;
  }

 private:
  /// One prompt-loop iteration; returns true when the REPL should exit.
  bool HandleLine(const std::string& line) {
    repl_ns::Action action = parser_.Parse(line);
    if (action.kind == repl_ns::Action::Kind::kRequest && action.method == "ReadStatusResource") {
      std::string robot = action.params.value("robot_name", "");
      if (robot.empty()) {
        const auto robots = catalog_.Robots();
        if (robots.size() != 1) {
          Print(robots.empty()
                    ? "status resource unavailable: refresh discovery or provide a robot name"
                    : "status resource is ambiguous: provide a robot name");
          return false;
        }
        robot = robots.front();
      }
      action.method = "ReadResource";
      action.params = {{"uri", "robots/" + robot + "/" +
                                   action.params.at("class").get<std::string>() + "/status"}};
    }
    if (action.kind == repl_ns::Action::Kind::kRequest && action.method == "StartExecution") {
      std::string error;
      if (!PrepareSkillStart(&action, &error)) {
        Print("validation error: " + error);
        return false;
      }
    }
    if (action.kind != repl_ns::Action::Kind::kNone &&
        action.kind != repl_ns::Action::Kind::kError &&
        !(action.kind == repl_ns::Action::Kind::kRequest && action.prompt_json)) {
      terminal_.AddHistory(line);
    }
    switch (action.kind) {
      case repl_ns::Action::Kind::kNone:
        return false;
      case repl_ns::Action::Kind::kError:
        Print(action.error);
        return false;
      case repl_ns::Action::Kind::kHelp:
        PrintHelp(action.help_topic);
        return false;
      case repl_ns::Action::Kind::kQuit:
        if (args_.eosd_pid > 0) exit_code_ = kQuitRequested;
        return true;
      case repl_ns::Action::Kind::kLeave:
        if (args_.eosd_pid > 0) exit_code_ = kLeaveRequested;
        return true;
      case repl_ns::Action::Kind::kShutdown:
        if (args_.eosd_pid <= 0) {
          Print("shutdown unavailable: eos-repl was not started with --eosd-pid");
          return false;
        }
        if (::kill(args_.eosd_pid, SIGTERM) != 0 && errno != ESRCH) {
          Print("shutdown failed: " + std::string(std::strerror(errno)));
          return false;
        }
        Print("shutdown requested; exiting REPL");
        return true;
      case repl_ns::Action::Kind::kRebuild:
        if (args_.eosd_pid <= 0) {
          Print("rebuild unavailable: eos-repl is not managed by eos-launch.sh");
          return false;
        }
        Print("requesting eosd and REPL rebuild/restart; prelaunch will remain running");
        exit_code_ = kRebuildRequested;
        return true;
      case repl_ns::Action::Kind::kConnect:
        Connect(action.host.empty() ? args_.host : action.host,
                action.port == 0 ? args_.port : action.port, action.host);
        return false;
      case repl_ns::Action::Kind::kDisconnect:
        session_.Close();  // OnClose prints the notice
        catalog_.ClearRemote();
        SetRobotIdentity("offline");
        return false;
      case repl_ns::Action::Kind::kRequest:
        if (action.prompt_json) {
          json arguments;
          if (!(action.guided_prompt
                    ? ReadGuidedArguments(action, &arguments)
                    : ReadJsonArguments(&arguments,
                                        action.method == "ApplyActionList"
                                            ? R"({"definition":{"workflowId":"","actions":[]}})"
                                            : "{}")))
            return false;
          if (action.method == "ApplyActionList" || action.method == "InferVad" ||
              action.method == "ObserveObjects" ||
              action.method == "DetectHumans" || action.method == "EstimateHumanPose" ||
              action.method == "DetectFaces")
            action.params = arguments;
          else if (action.method == "SignalExecution")
            action.params["payload"] = arguments;
          else
            action.params["arguments"] = arguments;
          std::string validation_error;
          if (!ValidateAction(action, &validation_error)) {
            Print("validation error: " + validation_error);
            return false;
          }
          Print("request preview: " + action.params.dump(2));
          std::string confirmation;
          if (terminal_.Read("send request? [y/N] ", &confirmation) !=
                  repl_ns::Terminal::ReadStatus::kLine ||
              (confirmation != "y" && confirmation != "Y" && confirmation != "yes")) {
            Print("request cancelled");
            return false;
          }
          terminal_.AddHistory(action.history_prefix + " --json " + arguments.dump());
        } else {
          std::string validation_error;
          if (!ValidateAction(action, &validation_error)) {
            Print("validation error: " + validation_error);
            return false;
          }
        }
        {
          const json result = Request(action.method, action.params);
          if (action.method == "StartExecution" && action.attach_execution &&
              !result.contains("error") && !IsTerminalExecution(result)) {
            const std::string execution_id = result.value("executionId", "");
            if (execution_id.empty())
              Print("cannot attach: start response has no executionId");
            else
              AttachExecution(execution_id);
          }
        }
        return false;
      case repl_ns::Action::Kind::kWatch:
        Watch(action.uri);
        return false;
      case repl_ns::Action::Kind::kWatchStream:
        WatchStream(action.stream_key, action.stream_path);
        return false;
      case repl_ns::Action::Kind::kWorkflowWatch:
        WatchWorkflow(action.uri);
        return false;
      case repl_ns::Action::Kind::kDescribe:
        Describe(action);
        return false;
      case repl_ns::Action::Kind::kRefresh:
        RefreshCatalog(true);
        return false;
      case repl_ns::Action::Kind::kAutoBase:
        AutoBase(action.params.at("device_id").get<std::string>());
        return false;
      case repl_ns::Action::Kind::kSetFull: {
        const std::lock_guard<std::mutex> lock(state_m_);
        display_.elide_b64 = !action.full;
      }
        Print(action.full ? "showing full payloads (set full off to elide)"
                          : "eliding base64 blobs (set full on to disable)");
        return false;
      case repl_ns::Action::Kind::kSetBrief: {
        const std::lock_guard<std::mutex> lock(state_m_);
        display_.brief = action.brief;
      }
        Print(action.brief ? "brief rendering enabled (set brief off for full views)"
                           : "brief rendering disabled");
        return false;
    }
    return false;
  }

  bool PrepareSkillStart(repl_ns::Action* action, std::string* error) {
    const json reference = action->params.value("reference", json::object());
    json arguments = action->params.value("arguments", json::object());
    std::string version;
    std::vector<std::string> unresolved;
    if (!catalog_.PrepareSkillLaunch(reference.value("name", ""), reference.value("version", ""),
                                     &arguments, &version, &unresolved, error))
      return false;
    action->params["reference"]["version"] = version;
    action->params["arguments"] = std::move(arguments);
    action->history_prefix = "start " + reference.value("name", "") + " " + version;
    if (!unresolved.empty() && !action->strict_json) {
      action->prompt_json = true;
      action->guided_prompt = true;
    }
    return true;
  }

  /// The friendly round-trip: send, block for the reply, render it.
  json Request(const std::string& method, const json& params) {
    const json result = session_.Call(method, params, args_.timeout_ms);
    catalog_.Observe(method, result);
    Print(method == "PsExecutions" ? repl_ns::RenderExecutionPs(result, params, DisplayNow())
                                   : repl_ns::RenderReply(method, result, DisplayNow()));
    return result;
  }

  void AutoBase(const std::string& device_id) {
    if (!terminal_.IsInteractive() || !session_.IsConnected()) {
      Print("auto base requires an interactive, connected terminal");
      return;
    }
    Print("auto base " + device_id + ": W/S forward/back, A/D left/right, "
          "J/K rotate, [ slower, ] faster; q or Ctrl-C exits (100%)");
    Print("auto base navigation: each key event updates the stream target");
    repl_ns::AutoBaseKeys keys;
    std::future<json> pending;
    bool pending_active = false;
    bool quit_requested = false;
    bool failed = false;
    auto failure_quiet_since = std::chrono::steady_clock::time_point{};
    std::optional<repl_ns::AutoBaseKeys::Nudge> latest;
    const auto begin_navigate = [&](repl_ns::AutoBaseKeys::Nudge nudge) {
      const json params = {{"name", "base.navigate"},
                           {"version", "1.0.0"},
                           {"device_id", device_id},
                           {"command_id", "repl-" + eos::lib::MintUuidV7()},
                           {"arguments", {{"reference", "relative"},
                                          {"stream", true},
                                          {"x_m", nudge.x_m},
                                          {"y_m", nudge.y_m},
                                          {"yaw_rad", nudge.yaw_rad}}}};
      pending = std::async(std::launch::async, [this, params] {
        return session_.Call("CallPrimitive", params, std::min(args_.timeout_ms, 25000));
      });
      pending_active = true;
    };
    terminal_.RunAutoKeys([&](int key) {
      const auto now = std::chrono::steady_clock::now();
      if (key == 'q' || key == 'Q' || key == 3 || key == -2) quit_requested = true;
      if (key >= 0 && !failed && !quit_requested) {
        if (keys.AdjustSpeed(static_cast<char>(key))) {
          Print("auto base speed: " + std::to_string(keys.SpeedPercent()) + "%");
        } else {
          const auto nudge = keys.Press(static_cast<char>(key));
          if (nudge.active()) latest = nudge;
        }
      }
      if (pending_active &&
          pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        pending_active = false;
        json result;
        try {
          result = pending.get();
        } catch (const std::exception& error) {
          result = {{"error", error.what()}};
        }
        if (result.contains("error") || result.value("accepted", true) == false) {
          Print("auto base navigate failed: " + result.dump());
          failed = true;
          latest.reset();
          failure_quiet_since = now;
        }
      }
      if (quit_requested) return false;
      if (failed) {
        if (key >= 0) failure_quiet_since = now;
        return key >= 0 || now - failure_quiet_since < std::chrono::milliseconds(650);
      }
      if (!pending_active && latest) {
        const auto nudge = *latest;
        latest.reset();
        begin_navigate(nudge);
      }
      return true;
    });
    if (pending_active) {
      try {
        const json result = pending.get();
        if (result.contains("error") || result.value("accepted", true) == false)
          Print("auto base navigate failed: " + result.dump());
      } catch (const std::exception& error) {
        Print(std::string("auto base navigate failed: ") + error.what());
      }
    }
    Print("auto base ended");
  }

  bool ReadJsonArguments(json* arguments, const std::string& initial = "{}") {
    const auto result = repl_ns::ReadJsonObject(
        [this](const std::string& preload, std::string* input) {
          return terminal_.Read("json> ", input, preload) == repl_ns::Terminal::ReadStatus::kLine;
        },
        [this](const std::string& error) { Print("error: " + error); }, initial);
    if (!result) {
      Print("JSON entry cancelled");
      return false;
    }
    *arguments = *result;
    return true;
  }

  static json ParseValue(const std::string& input, const std::string& type) {
    if (type == "string") return input;
    try {
      return json::parse(input);
    } catch (const json::parse_error&) {
      return input;
    }
  }

  std::map<std::string, repl_ns::ArgumentInfo> SchemaFor(const repl_ns::Action& action) const {
    if (action.method == "CallPrimitive")
      return catalog_.ArgumentsFor(action.params.value("name", ""),
                                   action.params.value("version", ""),
                                   action.params.value("device_id", ""));
    if (action.method == "StartExecution") {
      const json reference = action.params.value("reference", json::object());
      return catalog_.SkillArgumentsFor(reference.value("name", ""),
                                        reference.value("version", ""));
    }
    if (action.method == "SignalExecution")
      return catalog_.SignalArgumentsFor(action.params.value("execution_id", ""),
                                         action.params.value("name", ""));
    return {};
  }

  bool SchemaAvailableFor(const repl_ns::Action& action) const {
    if (action.method != "CallPrimitive") return !SchemaFor(action).empty();
    return catalog_.HasPrimitiveParameterSchema(action.params.value("name", ""),
                                                action.params.value("version", ""),
                                                action.params.value("device_id", ""));
  }

  bool ReadGuidedArguments(const repl_ns::Action& action, json* arguments) {
    if (action.method == "ApplyActionList") {
      json definition;
      std::string workflow_id;
      if (terminal_.Read("definition.workflowId (required string): ", &workflow_id) !=
              repl_ns::Terminal::ReadStatus::kLine ||
          workflow_id.empty()) {
        Print("validation error: definition.workflowId cannot be empty");
        return false;
      }
      std::string actions_text;
      if (terminal_.Read("definition.actions (required non-empty JSON array): ", &actions_text) !=
          repl_ns::Terminal::ReadStatus::kLine) {
        Print("request cancelled");
        return false;
      }
      json actions;
      try {
        actions = json::parse(actions_text);
      } catch (const json::parse_error& error) {
        Print(std::string("validation error: invalid actions JSON: ") + error.what());
        return false;
      }
      definition = {{"workflowId", workflow_id}, {"actions", std::move(actions)}};
      std::string include;
      if (terminal_.Read("include optional 'supersedesWorkflowIds'? [y/N] ", &include) ==
              repl_ns::Terminal::ReadStatus::kLine &&
          (include == "y" || include == "Y" || include == "yes")) {
        std::string supersedes_text;
        if (terminal_.Read("definition.supersedesWorkflowIds (JSON array): ", &supersedes_text) !=
            repl_ns::Terminal::ReadStatus::kLine) {
          Print("request cancelled");
          return false;
        }
        try {
          definition["supersedesWorkflowIds"] = json::parse(supersedes_text);
        } catch (const json::parse_error& error) {
          Print(std::string("validation error: invalid supersedes JSON: ") + error.what());
          return false;
        }
      }
      *arguments = {{"definition", std::move(definition)}};
      return true;
    }
    const auto schema = SchemaFor(action);
    if (schema.empty() && !SchemaAvailableFor(action)) {
      Print("no parameter schema is available; opening strict JSON entry");
      return ReadJsonArguments(arguments);
    }
    if (schema.empty()) {
      *arguments = json::object();
      return true;
    }
    json result =
        action.method == "StartExecution"    ? action.params.value("arguments", json::object())
        : action.method == "SignalExecution" ? action.params.value("payload", json::object())
                                             : json::object();
    for (const bool required_pass : {true, false}) {
      for (const auto& [name, info] : schema) {
        if (info.required != required_pass) continue;
        if (result.contains(name)) continue;
        if (!info.required) {
          std::string include;
          if (terminal_.Read("include optional '" + name + "'? [y/N] ", &include) !=
                  repl_ns::Terminal::ReadStatus::kLine ||
              (include != "y" && include != "Y" && include != "yes"))
            continue;
        }
        std::string hint = name + " (" + (info.type.empty() ? "value" : info.type) + ")";
        if (!info.values.empty()) {
          hint += " [";
          for (std::size_t i = 0; i < info.values.size(); ++i)
            hint += (i == 0 ? "" : "|") + info.values[i];
          hint += "]";
        }
        if (info.has_default) hint += " default=" + info.default_value.dump();
        const std::string limit = LimitHint(info);
        if (!limit.empty()) hint += " " + limit;
        if (!info.description.empty()) hint += " — " + info.description;
        std::string value;
        if (terminal_.Read(hint + ": ", &value) != repl_ns::Terminal::ReadStatus::kLine) {
          Print("request cancelled");
          return false;
        }
        if (value.empty() && info.has_default) {
          result[name] = info.default_value;
        } else if (value.empty() && info.required) {
          Print("validation error: required field '" + name + "' cannot be empty");
          return false;
        } else {
          result[name] = ParseValue(value, info.type);
        }
      }
    }
    std::string error;
    if (!catalog_.Validate(schema, result, &error)) {
      Print("validation error: " + error);
      return false;
    }
    *arguments = std::move(result);
    return true;
  }

  bool ValidateAction(const repl_ns::Action& action, std::string* error) const {
    if (action.method == "ApplyActionList") {
      return repl_ns::Catalog::ValidateWorkflow(action.params, error);
    }
    const auto schema = SchemaFor(action);
    if (schema.empty() && !SchemaAvailableFor(action)) return true;
    if (action.method == "CallPrimitive")
      return catalog_.Validate(schema, action.params.value("arguments", json::object()), error);
    if (action.method == "StartExecution")
      return catalog_.Validate(schema, action.params.value("arguments", json::object()), error);
    if (action.method == "SignalExecution")
      return catalog_.Validate(schema, action.params.value("payload", json::object()), error);
    return true;
  }

  static std::string LimitHint(const repl_ns::ArgumentInfo& info) {
    if (!info.schema.is_object()) return {};
    const bool minimum = info.schema.contains("minimum") && info.schema.at("minimum").is_number();
    const bool maximum = info.schema.contains("maximum") && info.schema.at("maximum").is_number();
    if (!minimum && !maximum) return {};
    return "range [" + (minimum ? info.schema.at("minimum").dump() : "-inf") + ", " +
           (maximum ? info.schema.at("maximum").dump() : "+inf") + "]";
  }

  void RefreshCatalog(bool announce) {
    catalog_.ClearRemote();
    if (!session_.IsConnected()) {
      if (announce) Print("[offline] cannot refresh completion catalog");
      return;
    }
    for (const std::string method :
         {"Discover", "ListResources", "ListSkills", "ListWorkflowTemplates", "ListExecutions"}) {
      const json result = session_.Call(method, nullptr, args_.timeout_ms);
      catalog_.Observe(method, result);
    }
    if (announce) {
      Print("completion catalog: " + std::to_string(catalog_.ResourceCount()) + " resources, " +
            std::to_string(catalog_.PrimitiveCount()) + " primitives, " +
            std::to_string(catalog_.WorkflowTemplates().size()) + " workflow templates");
    }
  }

  /// `watch` — a live resource stream that owns the terminal until `q`.
  void Watch(const std::string& uri) {
    std::string error;
    terminal_.BeginLiveView();
    if (!session_.Watch(
            uri,
            [this](const json& resource) {
              auto display = DisplayNow();
              display.brief = true;
              terminal_.Redraw(repl_ns::RenderReply("ReadResource", resource, display));
            },
            &error)) {
      terminal_.EndLiveView();
      Print("error: " + error);
      return;
    }
    terminal_.WaitForWatchQuit();
    session_.Unwatch(uri);
    terminal_.EndLiveView();
  }

  /// Shows one Skill execution/event SSE stream until q is pressed.
  void WatchStream(const std::string& key, const std::string& path) {
    std::string error;
    auto monitored = std::make_shared<std::map<std::string, json>>();
    if (!session_.WatchStream(
            key, path,
            [this, key, monitored](const std::string& event, const json& payload) {
              if (key == "ps" && event == "skill-monitor-update") {
                if (payload.value("reset", false)) monitored->clear();
                for (const auto& id : payload.value("removedExecutionIds", json::array()))
                  if (id.is_string()) monitored->erase(id.get<std::string>());
                for (const auto& execution : payload.value("upserted", json::array()))
                  if (execution.is_object())
                    (*monitored)[execution.value("executionId", "")] = execution;
                json executions = json::array();
                for (const auto& entry : *monitored) executions.push_back(entry.second);
                terminal_.Redraw(repl_ns::RenderExecutionPs({{"executions", std::move(executions)}},
                                                            json::object(), DisplayNow()));
                return true;
              }
              terminal_.Redraw("[" + event + "]\n" + repl_ns::FormatResult(payload, DisplayNow()));
              return true;
            },
            &error)) {
      Print("error: " + error);
      return;
    }
    terminal_.WaitForWatchQuit();
    session_.Unwatch(key);
  }

  void WatchWorkflow(const std::string& workflow_id) {
    const json snapshot = session_.Call("GetRuntimeSnapshot", nullptr, args_.timeout_ms);
    if (snapshot.contains("error")) {
      Print(repl_ns::RenderReply("GetRuntimeSnapshot", snapshot, DisplayNow()));
      return;
    }
    const json cursor = snapshot.value("actionListCursor", json::object());
    const std::string epoch = cursor.value("epoch", "");
    if (epoch.empty()) {
      Print("error: runtime snapshot has no actionListCursor.epoch");
      return;
    }
    std::string sequence;
    if (!CursorSequence(cursor.value("sequence", json("0")), &sequence)) {
      Print("error: runtime snapshot has an invalid actionListCursor.sequence");
      return;
    }
    if (workflow_id.empty()) {
      WatchStream("workflow", "/v1/eos/workflows:watch?cursor=" +
                                   UrlEncode(epoch + ":" + sequence));
      return;
    }

    json initial;
    for (const auto& action_list : snapshot.value("actionLists", json::array())) {
      if (action_list.is_object() && action_list.value("workflowId", "") == workflow_id) {
        initial = action_list;
        break;
      }
    }
    if (initial.empty()) {
      Print("error: workflow not found: " + workflow_id);
      return;
    }

    const std::string key = "workflow:" + workflow_id;
    std::string error;
    terminal_.BeginLiveView();
    terminal_.Redraw(repl_ns::FormatResult(initial, DisplayNow()),
                     "[q] detach  [Ctrl-C] terminate workflow");
    if (!session_.WatchStream(
            key, "/v1/eos/workflows:watch?cursor=" + UrlEncode(epoch + ":" + sequence),
            [this, workflow_id](const std::string& event, const json& payload) {
              if (event != "action-list-event") return true;
              const json action_list = payload.value("actionList", json::object());
              if (action_list.value("workflowId", "") != workflow_id) return true;
              terminal_.Redraw(repl_ns::FormatResult(action_list, DisplayNow()),
                               "[q] detach  [Ctrl-C] terminate workflow");
              return true;
            },
            &error)) {
      terminal_.EndLiveView();
      Print("error: " + error);
      return;
    }

    const auto control = terminal_.WaitForWorkflowWatchControl();
    session_.Unwatch(key);
    terminal_.EndLiveView();
    if (control != repl_ns::Terminal::WorkflowWatchControl::kCancel) return;

    const std::string replacement_id = "repl-stop-" + eos::lib::MintUuidV7();
    const json request = {
        {"definition",
         {{"workflowId", replacement_id},
          {"actions",
           {{{"actionId", "supersede"}, {"noop", json::object()}, {"parameters", json::object()}}}},
          {"supersedesWorkflowIds", {workflow_id}}}}};
    const json result = session_.Call("ApplyActionList", request, args_.timeout_ms);
    Print(repl_ns::RenderReply("ApplyActionList", result, DisplayNow()));
    if (!result.contains("error"))
      Print("termination requested for workflow " + workflow_id);
  }

  static bool IsTerminalExecution(const json& execution) {
    const std::string encoded = execution.value("status", "");
    const std::string status =
        encoded.rfind("EXECUTION_STATUS_", 0) == 0 ? encoded.substr(17) : encoded;
    return status == "SUCCEEDED" || status == "REJECTED" || status == "CANCELLED" ||
           status == "TIMED_OUT" || status == "FAILED";
  }

  void AttachExecution(const std::string& execution_id) {
    const std::string key = "attached:" + execution_id;
    const std::string path = "/v1/eos/executions/" + execution_id + ":watch";
    auto terminal = std::make_shared<std::atomic_bool>(false);
    auto prompting = std::make_shared<std::atomic_bool>(false);
    const auto redraw = [this, terminal, prompting](const std::string& event, const json& payload) {
      if (!prompting->load())
        terminal_.Redraw("[" + event + "]\n" + repl_ns::FormatResult(payload, DisplayNow()) +
                             "\n[Ctrl-C] cancel  [p] pause  [r] resume  [s] signal  "
                             "[d/q] detach  [?] help",
                         "");
      if (IsTerminalExecution(payload)) terminal->store(true);
    };
    std::string error;
    if (!session_.WatchStream(
            key, path,
            [terminal, redraw](const std::string& event, const json& payload) {
              redraw(event, payload);
              if (event == "error") terminal->store(true);
              return !terminal->load();
            },
            &error)) {
      Print("error: " + error);
      return;
    }
    Print("attached to " + execution_id +
          "; Ctrl-C cancel, p pause, r resume, s signal, d/q detach, ? help");
    auto next_refresh = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!terminal->load()) {
      const auto control = terminal_.WaitForAttachedControl(
          [&] { return terminal->load(); },
          [&] {
            const auto now = std::chrono::steady_clock::now();
            if (prompting->load() || now < next_refresh) return;
            next_refresh = now + std::chrono::milliseconds(500);
            const json current = session_.Call("GetExecution", {{"execution_id", execution_id}},
                                               std::min(args_.timeout_ms, 500));
            if (!current.contains("error")) redraw("skill-execution", current);
          });
      if (control == repl_ns::Terminal::AttachedControl::kTerminal) break;
      if (control == repl_ns::Terminal::AttachedControl::kDetach ||
          control == repl_ns::Terminal::AttachedControl::kHangup) {
        Print("detached from " + execution_id);
        break;
      }
      if (control == repl_ns::Terminal::AttachedControl::kHelp) {
        Print("attached controls: Ctrl-C cancel, p pause, r resume, s signal, d/q detach, ? help");
        continue;
      }
      if (control == repl_ns::Terminal::AttachedControl::kCancel) {
        prompting->store(true);
        HandleLine("sig cancel " + execution_id);
        Print("cancellation requested; detached from " + execution_id);
        break;
      }
      if (control == repl_ns::Terminal::AttachedControl::kPause ||
          control == repl_ns::Terminal::AttachedControl::kResume) {
        const std::string command =
            control == repl_ns::Terminal::AttachedControl::kPause ? "pause" : "resume";
        HandleLine("sig " + command + " " + execution_id);
        continue;
      }
      std::string signal;
      prompting->store(true);
      if (terminal_.Read("signal <name> [payload]> ", &signal) !=
              repl_ns::Terminal::ReadStatus::kLine ||
          signal.empty()) {
        prompting->store(false);
        continue;
      }
      prompting->store(false);
      const std::size_t split = signal.find_first_of(" \t");
      const std::string command =
          split == std::string::npos
              ? "sig " + signal + " " + execution_id
              : "sig " + signal.substr(0, split) + " " + execution_id + signal.substr(split);
      HandleLine(command);
    }
    session_.Unwatch(key);
  }

  /// Renders locally known primitive arguments and deployment limits.
  void Describe(const repl_ns::Action& action) {
    const std::string name = action.params.value("name", "");
    const std::string version = action.params.value("version", "");
    if (action.describe_kind == "skill") {
      const json result =
          Request("GetSkill", {{"reference", {{"name", name}, {"version", version}}}});
      if (result.contains("error")) return;
      const auto arguments = catalog_.SkillArgumentsFor(name, version);
      Print(name + " " + version + " skill parameters:");
      DescribeArguments(arguments, name, version, "");
      return;
    }
    const std::string device = action.params.value("device_id", "");
    const auto arguments = catalog_.ArgumentsFor(name, version, device);
    if (arguments.empty() && !catalog_.HasPrimitiveParameterSchema(name, version, device)) {
      Print("no local parameter schema for " + name + " " + version);
      return;
    }
    Print(name + " " + version + (device.empty() ? "" : " on " + device));
    if (arguments.empty()) {
      Print("  (no parameters)");
      return;
    }
    DescribeArguments(arguments, name, version, device);
  }

  void DescribeArguments(const std::map<std::string, repl_ns::ArgumentInfo>& arguments,
                         const std::string& name, const std::string& version,
                         const std::string& device) {
    for (const auto& [argument, info] : arguments) {
      std::string line = "  " + argument;
      line += info.required ? " (required" : " (optional";
      if (!info.type.empty()) line += ", " + info.type;
      line += ")";
      if (info.has_default) line += " default=" + info.default_value.dump();
      if (!info.values.empty()) {
        line += " choices=";
        for (std::size_t i = 0; i < info.values.size(); ++i)
          line += (i == 0 ? "" : ",") + info.values[i];
      }
      const std::string limit =
          device.empty() ? LimitHint(info) : catalog_.LimitHint(name, version, device, argument);
      if (!limit.empty()) line += " " + limit;
      if (!info.description.empty()) line += " — " + info.description;
      Print(line);
    }
  }

  /// Display settings as async paths should snapshot them (io thread).
  repl_ns::DisplayOptions DisplayNow() {
    const std::lock_guard<std::mutex> lock(state_m_);
    return display_;
  }

  /// (Re)connects with a user-visible outcome.
  void Connect(const std::string& host, std::uint16_t port, const std::string& note) {
    repl_ns::Session::Endpoint endpoint{host, port};
    std::string error;
    if (session_.Connect(endpoint, args_.connect_timeout_ms, &error)) {
      args_.host = host;
      args_.port = port;
      Print("connected to " + host + ":" + std::to_string(port));
      const json discovery = Request("Discover", nullptr);
      SetRobotIdentity(repl_ns::PromptIdentity(discovery));
      RefreshCatalog(false);
      return;
    }
    catalog_.ClearRemote();
    SetRobotIdentity("offline");
    Print("[offline] connect failed: " + error + (note.empty() ? "" : " (" + note + ")") +
          " — is eos-north-http running? try: connect [host] [port]");
  }

  void PrintHelp(const std::string& topic) {
    if (topic == "call") {
      Print("call <primitive> <version> <device|-> [key=value ...]");
      Print("call <primitive> <version> <device|-> --prompt | --json [{...}]");
      Print("Without an inline object, json> opens; Ctrl-J adds a line and Enter submits.");
      return;
    }
    if (topic == "capture") {
      Print("capture <robot-name> <camera-device-id> [1|2]");
      return;
    }
    if (topic == "history") {
      Print("history <uri> --sequence N");
      Print("history <uri> --before|--after|--nearest RFC3339-TIMESTAMP");
      Print("history <uri> --range FIRST LAST --limit N  (inclusive, limit 1..1000)");
      return;
    }
    if (!topic.empty() && !completion_.IsCommand(topic)) {
      Print("unknown help topic '" + topic + "'");
      return;
    }
    if (!topic.empty()) {
      int context_length = 0;
      const std::string usage = completion_.Hint(topic + " ", &context_length);
      Print(usage.empty() ? topic + " takes no arguments" : usage);
      return;
    }
    Print("commands (docs/eos/develop/interfaces/ is the contract):");
    Print("  discover                        Discover runtime and hardware");
    Print("  status                          GetStatus");
    Print("  audit-status                    external audit health and cursor");
    Print("  status base|arm|gripper [robot] Read normalized hardware status");
    Print("  waypoints <robot>              List stored navigation points");
    Print("  waypoint get <robot> <name>    Read one navigation point");
    Print("  waypoint put <robot> <name> <x> <y> <yaw>  Upsert a point");
    Print("  waypoint delete <robot> <name> Delete a navigation point");
    Print("  waypoint reload                Reload JSON edited outside EOS");
    Print("  capture <robot> <camera> [1|2] Capture bounded JPEG frames");
    Print("  resources | ls                  ListResources");
    Print("  read <uri>                      ReadResource");
    Print("  history <uri> <selector> ...    query retained resource products");
    Print("  watch <uri>                     Live resource view; press q to quit");
    Print("  refresh                         refresh completion metadata");
    Print("  call <primitive> <ver> <device|-> ... key=value, --json, or --prompt");
    Print("  auto base [device-id]           keyboard stream nudges; q or Ctrl-C exits");
    Print("  cancel-motion <command-id>     cancel an active primitive command");
    Print("  infer-vad --json [{...}]        typed audio VAD request (ProtoJSON bytes)");
    Print("  observe-objects --json [{...}] typed object observation request");
    Print("  perception-roles               list configured perception roles");
    Print("  detect-humans --json [{...}]    typed human detection request");
    Print("  estimate-human-pose --json [{...}] typed pose estimation request");
    Print("  detect-faces --json [{...}]     typed face detection request");
    Print("  skills | skill <name> <ver>     inspect the skill catalog");
    Print("  start <name> [ver] ...          launch and attach; --detach returns immediately");
    Print("  executions | execution <id>     list or inspect executions");
    Print("  state <id>                      inspect retained public execution state");
    Print("  watch-execution <id> [seq]      stream execution snapshots; q quits");
    Print("  watch-state <id> [revision]     stream public execution state; q quits");
    Print("  sig <signal> <id> ...           cancel, pause/resume, or declared signal");
    Print("  journalctl <id> [seq]           follow the execution journal; q quits");
    Print("  ps [skill [ver]] [--status S]   show active executions once");
    Print("     [--all] [--watch]             include terminal or refresh live; q quits");
    Print("  kill <id>                       cooperatively terminate one execution");
    Print("  pkill <skill> [version]         terminate matching active executions");
    Print("  workflows | workflow <id>       list or inspect submitted ActionLists");
    Print("  workflow apply [--json|--prompt] apply an ActionList definition");
    Print("  workflow watch [id]             watch workflows; Ctrl-C terminates a targeted one");
    Print("  workflow templates               list EOS-registered workflow templates");
    Print("  workflow show <template>         inspect a registered template");
    Print("  workflow run <template> [id]     submit a fresh template copy for debugging");
    Print("  runtime                         manager runtime snapshot");
    Print("  describe primitive|skill ...     show required/default/type/choices/limits");
    Print("  set full on|off                 base64 blobs verbatim (default) vs elided");
    Print("  set brief on|off                concise known resources and state views");
    Print("  connect [host] [port]           (re)connect to the north HTTP");
    Print("  disconnect                      close the session, stay here");
    Print("  rebuild                         rebuild/restart eosd + REPL; preserve prelaunch");
    Print("  shutdown                        stop eosd, prelaunch, and REPL");
    Print("  leave                           exit REPL, return to shell; keep EOS running");
    Print("  help [command] | quit           help | exit (Ctrl-D works too)");
    Print("keys: Tab complete, Ctrl-R history, Ctrl-C cancel, Ctrl-J JSON newline");
  }

  /// Thread-safe line print (main + io threads).
  void Print(const std::string& text) { terminal_.Print(text); }

  /// Re-issues the prompt after async output so pushed lines stay readable.
  std::string PromptText() {
    std::string robot_identity;
    {
      const std::lock_guard<std::mutex> lock(state_m_);
      robot_identity = robot_identity_;
    }
    return "Aether-eos@" + robot_identity + "> ";
  }

  void SetRobotIdentity(std::string robot_identity) {
    const std::lock_guard<std::mutex> lock(state_m_);
    robot_identity_ = std::move(robot_identity);
    terminal_.SetPrompt("Aether-eos@" + robot_identity_ + "> ");
  }

  Args args_;
  repl_ns::CommandParser parser_;
  repl_ns::DisplayOptions display_;
  repl_ns::Session session_;
  repl_ns::Catalog catalog_;
  repl_ns::CompletionEngine completion_;
  repl_ns::Terminal terminal_;
  std::mutex state_m_;
  std::string robot_identity_ = "offline";
  int exit_code_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  try {
    eos::lib::ConfigureLogging(ReplLogRoot(), "repl", eos::lib::LogConfig{});
    auto log = eos::lib::GetLogger("eos-repl");
    eos::lib::LogEvent(log, spdlog::level::info, "repl_process_started");
    bool help_requested = false;
    const Args args = repl_ns::ParseArgs(argc, argv, &help_requested);
    if (help_requested) {
      std::fprintf(stderr, "%s", repl_ns::Usage());
      return 0;
    }
    App app(args);
    const int result = app.Run();
    eos::lib::LogEvent(log, spdlog::level::info, "repl_process_stopped", {{"result", result}});
    return result;
  } catch (const std::exception& error) {
    eos::lib::LogEvent(eos::lib::GetLogger("eos-repl"), spdlog::level::err, "repl_process_failed",
                       {{"reason", error.what()}});
    std::fprintf(stderr, "eos-repl: %s\n", error.what());
    return 1;
  }
}
