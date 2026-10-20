/// Northbound REPL output rendering — see format.h.

#include "format.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <vector>

namespace eos::ext::repl {
namespace {

using json = nlohmann::json;

/// Blobs at or past this length collapse to a placeholder when eliding —
/// short strings (tones, ids) never do.
constexpr std::size_t kElideAbove = 120;
constexpr const char* kReset = "\033[0m";
constexpr const char* kHeading = "\033[1;36m";
constexpr const char* kLabel = "\033[1;34m";
constexpr const char* kString = "\033[32m";
constexpr const char* kNumber = "\033[36m";
constexpr const char* kPositive = "\033[32m";
constexpr const char* kNegative = "\033[31m";
constexpr const char* kNull = "\033[90m";

std::int64_t JsonInt64(const json& value) {
  if (value.is_number_integer()) return value.get<std::int64_t>();
  if (value.is_string()) {
    try {
      return std::stoll(value.get<std::string>());
    } catch (const std::exception&) {
    }
  }
  return 0;
}

std::int64_t DurationMilliseconds(const json& duration) {
  if (duration.is_object()) {
    return JsonInt64(duration.value("seconds", json(0))) * 1000 +
           JsonInt64(duration.value("nanos", json(0))) / 1000000;
  }
  if (!duration.is_string()) return 0;
  const std::string encoded = duration.get<std::string>();
  if (encoded.size() < 2 || encoded.back() != 's') return 0;
  try {
    return static_cast<std::int64_t>(std::stod(encoded.substr(0, encoded.size() - 1)) * 1000);
  } catch (const std::exception&) {
    return 0;
  }
}

std::string ActivityName(const json& activity) {
  if (activity.is_object()) return activity.value("name", "");
  return activity.is_string() ? activity.get<std::string>() : "";
}

/// True when the key marks a base64 payload, including canonical camera JPEG bytes.
bool IsB64Key(const std::string& key) {
  return key == "jpeg" || (key.size() >= 4 && key.compare(key.size() - 4, 4, "_b64") == 0);
}

/// Recursively copies `value`, collapsing elidable blobs to a length note.
json Elide(const json& value) {
  if (value.is_object()) {
    json out = json::object();
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (it->is_string() && IsB64Key(it.key()) &&
          it->get_ref<const std::string&>().size() >= kElideAbove) {
        out[it.key()] =
            "<" + std::to_string(it->get_ref<const std::string&>().size()) + " chars b64>";
      } else {
        out[it.key()] = Elide(*it);
      }
    }
    return out;
  }
  if (value.is_array()) {
    json out = json::array();
    for (const auto& item : value) {
      out.push_back(Elide(item));
    }
    return out;
  }
  return value;
}

std::string Scalar(const json& value) {
  return value.is_string() ? value.get<std::string>() : value.dump();
}

void AppendBriefField(const json& value, const char* key, const char* label, std::string* out) {
  const auto found = value.find(key);
  if (found == value.end() || found->is_null()) return;
  if (!out->empty() && out->back() != '\n') *out += ' ';
  *out += std::string(label) + "=" + Scalar(*found);
}

std::string BriefResources(const json& value) {
  if (!value.is_array()) return {};
  if (value.empty()) return "(none)";
  std::string out;
  for (const auto& resource : value) {
    if (!resource.is_object() || !resource.contains("uri")) return {};
    if (!out.empty()) out += '\n';
    out += resource.value("class", std::string("resource")) + "  " +
           Scalar(resource.at("uri"));
  }
  return out;
}

std::string BriefDetection(const json& value) {
  std::string out;
  AppendBriefField(value, "source", "source", &out);
  AppendBriefField(value, "source_sequence", "source-seq", &out);
  AppendBriefField(value, "source_generation", "source-gen", &out);
  AppendBriefField(value, "sequence", "seq", &out);
  const json models = value.value("models", json::object());
  if (models.is_object()) {
    for (const char* name : {"human", "pose", "action", "face"}) {
      const auto found = models.find(name);
      if (found == models.end() || !found->is_object()) continue;
      if (!out.empty()) out += '\n';
      out += std::string(name);
      AppendBriefField(*found, "status", "status", &out);
      AppendBriefField(*found, "sequence", "seq", &out);
      AppendBriefField(*found, "source_generation", "source-gen", &out);
      AppendBriefField(*found, "requested_generation", "requested-gen", &out);
    }
  }
  const json detections = value.value("detections", json::array());
  if (!out.empty()) out += ' ';
  out += "detections=" + std::to_string(detections.is_array() ? detections.size() : 0);
  if (!detections.is_array()) return out;
  for (const auto& detection : detections) {
    if (!detection.is_object()) continue;
    out += '\n';
    AppendBriefField(detection, "id", "id", &out);
    AppendBriefField(detection, "kind", "kind", &out);
    AppendBriefField(detection, "label", "label", &out);
    AppendBriefField(detection, "confidence", "confidence", &out);
    AppendBriefField(detection, "distance_m", "distance-m", &out);
    AppendBriefField(detection, "bearing_rad", "bearing-rad", &out);
    const json attributes = detection.value("attributes", json::object());
    if (attributes.is_object()) {
      AppendBriefField(attributes, "name", "name", &out);
      AppendBriefField(attributes, "pose", "pose", &out);
      AppendBriefField(attributes, "posture_state", "posture-state", &out);
      AppendBriefField(attributes, "action", "action", &out);
      AppendBriefField(attributes, "activity", "activity", &out);
    }
  }
  return out;
}

std::string BriefArm(const json& value) {
  std::string out;
  AppendBriefField(value, "part_id", "part", &out);
  AppendBriefField(value, "sequence", "seq", &out);
  AppendBriefField(value, "pose_valid", "pose-valid", &out);
  const json joints = value.value("joint_positions_rad", json::array());
  if (joints.is_array()) {
    if (!out.empty()) out += ' ';
    out += "joints=" + joints.dump();
  }
  return out;
}

std::string BriefDevice(const json& value) {
  std::string out;
  AppendBriefField(value, "id", "device", &out);
  if (!value.contains("id")) AppendBriefField(value, "device_id", "device", &out);
  if (!value.contains("id") && !value.contains("device_id"))
    AppendBriefField(value, "deviceId", "device", &out);
  AppendBriefField(value, "part_id", "part", &out);
  AppendBriefField(value, "device_class", "class", &out);
  if (!value.contains("device_class")) AppendBriefField(value, "class", "class", &out);
  const json state = value.value("operational_state", json::object());
  if (state.is_object()) {
    AppendBriefField(state, "controller_state", "state", &out);
    AppendBriefField(state, "motors_enabled", "motors", &out);
    AppendBriefField(state, "calibrated", "calibrated", &out);
    AppendBriefField(state, "active", "active", &out);
  }
  const auto direct_state = value.find("state");
  if (direct_state != value.end() && !direct_state->is_structured())
    AppendBriefField(value, "state", "state", &out);
  AppendBriefField(value, "status", "status", &out);
  AppendBriefField(value, "health", "health", &out);
  return out;
}

std::string BriefVad(const json& value) {
  std::string out;
  AppendBriefField(value, "speech", "speech", &out);
  AppendBriefField(value, "probability", "probability", &out);
  const auto segments = value.find("segments");
  if (segments != value.end() && segments->is_array()) {
    if (!out.empty()) out += ' ';
    out += "segments=" + std::to_string(segments->size());
  }
  return out;
}

std::string BriefKnown(const std::string& method, const json& value);

std::string BriefHistory(const json& value) {
  const auto products = value.find("products");
  if (products == value.end() || !products->is_array()) return {};
  std::string out;
  AppendBriefField(value, "uri", "uri", &out);
  if (!out.empty()) out += ' ';
  out += "products=" + std::to_string(products->size());
  const json retention = value.value("retention", json::object());
  if (retention.is_object()) {
    AppendBriefField(retention, "firstAvailableSequence", "first", &out);
    AppendBriefField(retention, "latestSequence", "latest", &out);
    AppendBriefField(retention, "evictedThroughSequence", "evicted-through", &out);
    AppendBriefField(retention, "closed", "closed", &out);
  }
  AppendBriefField(value, "retentionGap", "retention-gap", &out);
  AppendBriefField(value, "truncated", "truncated", &out);
  for (const auto& product : *products) {
    if (!product.is_object()) continue;
    out += '\n';
    AppendBriefField(product, "sequence", "seq", &out);
    AppendBriefField(product, "observedAt", "observed-at", &out);
    AppendBriefField(product, "bootId", "boot", &out);
    AppendBriefField(product, "epoch", "epoch", &out);
    const auto payload = product.find("value");
    if (payload == product.end()) continue;
    const std::string brief = BriefKnown("", *payload);
    if (!brief.empty()) {
      out += " " + brief;
    } else {
      out += " value=" + payload->dump();
    }
  }
  return out;
}

std::string BriefExecution(const json& value) {
  std::string out;
  const json reference = value.value("reference", json::object());
  if (reference.is_object() && reference.contains("name")) {
    out = "skill=" + reference.value("name", "");
    const std::string version = reference.value("version", "");
    if (!version.empty()) out += "@" + version;
  }
  AppendBriefField(value, value.contains("executionId") ? "executionId" : "execution_id",
                   "execution", &out);
  AppendBriefField(value, "status", "state", &out);
  const std::string activity = ActivityName(value.value("currentActivity", json::object()));
  if (!activity.empty()) {
    if (!out.empty()) out += ' ';
    out += "activity=" + activity;
  }
  AppendBriefField(value, value.contains("reasonCode") ? "reasonCode" : "reason_code", "reason",
                   &out);
  AppendBriefField(value, "message", "message", &out);
  return out;
}

std::string RenderBody(const json& value, bool color);

std::string BriefExecutionState(const json& value) {
  if (!value.contains("revision") || !value.contains("payload") ||
      !value.at("payload").is_object())
    return {};
  std::string out;
  AppendBriefField(value, value.contains("executionId") ? "executionId" : "execution_id",
                   "execution", &out);
  AppendBriefField(value, "revision", "revision", &out);
  AppendBriefField(value, value.contains("publishedAt") ? "publishedAt" : "published_at",
                   "published-at", &out);
  std::string payload = BriefKnown("", value.at("payload"));
  if (payload.empty()) payload = RenderBody(value.at("payload"), false);
  if (!payload.empty()) out += "\n" + payload;
  return out;
}

std::string BriefKnown(const std::string& method, const json& value) {
  if (method == "ListResources") return BriefResources(value);
  if (!value.is_object()) return {};
  if (method == "QueryResourceHistory") return BriefHistory(value);
  if (method == "GetExecutionState" ||
      (value.contains("revision") && value.contains("payload")))
    return BriefExecutionState(value);
  const std::string type = value.value("type", "");
  if (type == "detection.snapshot") return BriefDetection(value);
  if (type == "arm.snapshot") return BriefArm(value);
  if (type == "asr.snapshot") {
    std::string out;
    AppendBriefField(value, "text", "text", &out);
    AppendBriefField(value, "language", "language", &out);
    AppendBriefField(value, "confidence", "confidence", &out);
    AppendBriefField(value, "sequence", "seq", &out);
    return out;
  }
  if (type == "vad.snapshot" ||
      (value.contains("speech") && value.contains("probability") && value.contains("segments")))
    return BriefVad(value);
  if (type == "body.snapshot" || type == "device.snapshot" || value.contains("device_class") ||
      (value.contains("operational_state") && value.contains("class")))
    return BriefDevice(value);
  if ((value.contains("executionId") || value.contains("execution_id") ||
       value.contains("reference")) &&
      value.contains("status"))
    return BriefExecution(value);
  return {};
}

void AppendIndent(int width, std::string* out) {
  out->append(static_cast<std::size_t>(width), ' ');
}

void AppendColored(const char* color, const std::string& text, std::string* out) {
  *out += color;
  *out += text;
  *out += kReset;
}

std::string WordLabel(const std::string& word) {
  if (word == "id") return "ID";
  if (word == "uri") return "URI";
  if (word == "eos") return "EOS";
  if (word == "abi") return "ABI";
  if (word == "fov") return "FOV";
  if (word == "pcm") return "PCM";
  if (word == "b64") return "Base64";
  if (word == "hz") return "Hz";
  if (word == "px") return "px";
  if (word == "pct") return "%";
  if (word.empty()) return word;
  std::string out = word;
  out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
  return out;
}

std::string HumanLabel(const std::string& key) {
  struct UnitSuffix {
    const char* suffix;
    const char* unit;
  };
  constexpr UnitSuffix units[] = {
      {"_seconds", "s"}, {"_rad_s", "rad/s"}, {"_m_s", "m/s"}, {"_ms", "ms"}, {"_rad", "rad"},
      {"_pct", "%"},     {"_hz", "Hz"},       {"_px", "px"},   {"_m", "m"},   {"_s", "s"},
      {"_v", "V"},       {"_a", "A"},         {"_n", "N"}};
  std::string name;
  name.reserve(key.size());
  for (const char c : key) {
    if (std::isupper(static_cast<unsigned char>(c)) != 0 && !name.empty()) name += '_';
    name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  std::string unit;
  for (const auto& candidate : units) {
    const std::string suffix = candidate.suffix;
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
      name.erase(name.size() - suffix.size());
      unit = candidate.unit;
      break;
    }
  }

  std::string out;
  std::size_t begin = 0;
  while (begin <= name.size()) {
    const std::size_t end = name.find('_', begin);
    if (!out.empty()) out += ' ';
    out += WordLabel(name.substr(begin, end == std::string::npos ? end : end - begin));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  if (!unit.empty()) out += " (" + unit + ")";
  return out;
}

std::string HumanEnum(std::string value) {
  constexpr const char* prefixes[] = {"RUNTIME_HEALTH_STATE_", "PRIMITIVE_TIER_",
                                      "SKILL_REGISTRATION_DISPOSITION_"};
  for (const char* prefix : prefixes) {
    const std::string prefix_text = prefix;
    if (value.rfind(prefix_text, 0) == 0) {
      value.erase(0, prefix_text.size());
      for (char& c : value) {
        c = c == '_' ? ' ' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      break;
    }
  }
  return value;
}

void AppendValue(const json& value, bool color, std::string* out) {
  std::string text;
  const char* style = nullptr;
  if (value.is_null()) {
    text = "-";
    style = kNull;
  } else if (value.is_boolean()) {
    text = value.get<bool>() ? "yes" : "no";
    style = value.get<bool>() ? kPositive : kNegative;
  } else if (value.is_string()) {
    const std::string& raw = value.get_ref<const std::string&>();
    text = raw.empty() ? "(empty)" : HumanEnum(raw);
    style = raw.empty() ? kNull : kString;
  } else {
    text = value.dump();
    style = kNumber;
  }
  if (color) {
    AppendColored(style, text, out);
  } else {
    *out += text;
  }
}

bool IsScalarArray(const json& value) {
  for (const auto& item : value) {
    if (item.is_structured()) return false;
  }
  return true;
}

void RenderFields(const json& value, int indent, bool color, std::string* out);

void RenderField(const std::string& key, const json& value, int indent, bool color,
                 std::string* out) {
  AppendIndent(indent, out);
  const std::string label = HumanLabel(key);
  if (color) {
    AppendColored(kLabel, label, out);
  } else {
    *out += label;
  }

  if (value.is_object()) {
    *out += value.empty() ? ": (none)\n" : "\n";
    if (!value.empty()) RenderFields(value, indent + 2, color, out);
    return;
  }
  if (value.is_array()) {
    *out += " (" + std::to_string(value.size()) + ")";
    if (value.empty()) {
      *out += ": (none)\n";
    } else if (IsScalarArray(value)) {
      *out += ": ";
      for (std::size_t i = 0; i < value.size(); ++i) {
        if (i != 0) *out += ", ";
        AppendValue(value.at(i), color, out);
      }
      *out += '\n';
    } else {
      *out += '\n';
      for (std::size_t i = 0; i < value.size(); ++i) {
        AppendIndent(indent + 2, out);
        *out += "[" + std::to_string(i + 1) + "]\n";
        if (value.at(i).is_object()) {
          RenderFields(value.at(i), indent + 4, color, out);
        } else {
          if (value.at(i).is_array()) {
            RenderField("items", value.at(i), indent + 4, color, out);
          } else {
            AppendIndent(indent + 4, out);
            AppendValue(value.at(i), color, out);
            *out += '\n';
          }
        }
      }
    }
    return;
  }
  *out += ": ";
  AppendValue(value, color, out);
  *out += '\n';
}

void RenderFields(const json& value, int indent, bool color, std::string* out) {
  static const std::vector<std::string> preferred = {"robot_id",
                                                     "id",
                                                     "uri",
                                                     "name",
                                                     "reference",
                                                     "type",
                                                     "schema_version",
                                                     "status",
                                                     "mode",
                                                     "accepted",
                                                     "health",
                                                     "reason_code",
                                                     "reason",
                                                     "command_id",
                                                     "sequence",
                                                     "timestamp",
                                                     "description",
                                                     "eos",
                                                     "hardware",
                                                     "backend",
                                                     "device_class",
                                                     "class",
                                                     "part_id",
                                                     "model",
                                                     "vendor",
                                                     "version",
                                                     "tier",
                                                     "state_uri",
                                                     "state",
                                                     "result",
                                                     "power",
                                                     "frame",
                                                     "semantics",
                                                     "bounds",
                                                     "vision",
                                                     "primitives",
                                                     "execution_id",
                                                     "latest_journal_sequence",
                                                     "code",
                                                     "message",
                                                     "primitive_acks",
                                                     "robots",
                                                     "devices"};
  std::vector<std::string> keys;
  keys.reserve(value.size());
  for (auto it = value.begin(); it != value.end(); ++it) keys.push_back(it.key());
  std::stable_sort(
      keys.begin(), keys.end(), [&](const std::string& left, const std::string& right) {
        const auto left_position = std::find(preferred.begin(), preferred.end(), left);
        const auto right_position = std::find(preferred.begin(), preferred.end(), right);
        const auto left_rank =
            left_position == preferred.end() ? preferred.size() : left_position - preferred.begin();
        const auto right_rank = right_position == preferred.end()
                                    ? preferred.size()
                                    : right_position - preferred.begin();
        return left_rank == right_rank ? left < right : left_rank < right_rank;
      });
  for (const auto& key : keys) {
    RenderField(key, value.at(key), indent, color, out);
  }
}

std::string ResponseTitle(const std::string& method, const json& reply) {
  if (reply.is_object() && reply.contains("error")) return "Error";
  if (method == "Discover") return "EOS Discovery";
  if (method == "GetStatus") return "Runtime Status";
  if (method == "GetAuditStatus") return "Audit Status";
  if (method == "ListResources") return "Resources";
  if (method == "QueryResourceHistory") return "Resource History";
  if (method == "CaptureFrames") return "Camera Frames";
  if (method == "InferVad") return "Voice Activity Detection";
  if (method == "ObserveObjects") return "Object Observation";
  if (method == "GetPerceptionRoles") return "Perception Roles";
  if (method == "DetectHumans") return "Human Detection";
  if (method == "EstimateHumanPose") return "Human Pose Estimation";
  if (method == "DetectFaces") return "Face Detection";
  if (method == "CallPrimitive") return "Primitive Release";
  if (method == "CancelPrimitive") return "Primitive Cancellation";
  if (method == "ListSkills") return "Skills";
  if (method == "GetSkill") return "Skill Description";
  if (method == "ListWorkflowTemplates") return "Workflow Templates";
  if (method == "GetWorkflowTemplate") return "Workflow Template";
  if (method == "RunWorkflowTemplate") return "Workflow Started";
  if (method == "StartExecution") return "Skill Execution Started";
  if (method == "ListExecutions") return "Skill Executions";
  if (method == "GetExecution") return "Skill Execution";
  if (method == "GetExecutionState") return "Skill Execution State";
  if (method == "TerminateExecution") return "Skill Termination";
  if (method == "TerminateExecutions") return "Skill Executions Terminated";
  if (method == "SignalExecution") return "Skill Signal Receipt";
  if (method == "ApplyActionList") return "Workflow Applied";
  if (method == "ListActionLists") return "Workflows";
  if (method == "GetActionList") return "Workflow";
  if (method == "GetRuntimeSnapshot") return "Manager Runtime Snapshot";
  if (method == "ReadResource" && reply.is_object()) {
    const std::string type = reply.value("type", "");
    if (type == "vision.snapshot") return "Vision Snapshot";
    if (type == "world.snapshot") return "World State";
    if (type == "detection.snapshot") return "Detection Snapshot";
    if (type == "asr.snapshot") return "Recognized Speech";
    if (type == "arm.snapshot") return "Arm State";
    if (reply.contains("robots") && reply.contains("devices")) return "Device Inventory";
    return "Resource";
  }
  return "Response";
}

std::string RenderBody(const json& value, bool color) {
  std::string out;
  if (value.is_object()) {
    if (value.empty()) return "(none)";
    RenderFields(value, 0, color, &out);
    if (!out.empty()) out.pop_back();
  } else if (value.is_array()) {
    RenderField("items", value, 0, color, &out);
    if (!out.empty()) out.pop_back();
  } else {
    AppendValue(value, color, &out);
  }
  return out;
}

}  // namespace

std::string FormatResult(const json& result, const DisplayOptions& options) {
  const json displayed = options.elide_b64 ? Elide(result) : result;
  if (options.brief) {
    const std::string brief = BriefKnown("", displayed);
    if (!brief.empty()) return brief;
  }
  return RenderBody(displayed, options.color);
}

std::string RenderExecutionPs(const json& reply, const json& filters,
                               const DisplayOptions& options) {
  if (!reply.is_object() || reply.contains("error"))
    return RenderReply("ListExecutions", reply, options);
  const std::string selected_name = filters.value("skill_name", "");
  const std::string selected_version = filters.value("skill_version", "");
  const json statuses = filters.value("statuses", json::array());
  const bool filter_status = !statuses.empty();
  std::ostringstream table;
  table << "EXECUTION                            SKILL@VERSION                 PROFILE       STATE       ACTIVITY       ELAPSED    REASON\n";
  std::size_t count = 0;
  for (const auto& execution : reply.value("executions", json::array())) {
    if (!execution.is_object()) continue;
    const json reference = execution.value("reference", json::object());
    if (!reference.is_object()) continue;
    const std::string name = reference.value("name", "");
    const std::string version = reference.value("version", "");
    const std::string status = execution.value("status", "");
    const std::string short_status =
        status.rfind("EXECUTION_STATUS_", 0) == 0 ? status.substr(17) : status;
    if ((!selected_name.empty() && name != selected_name) ||
        (!selected_version.empty() && version != selected_version))
      continue;
    if (filter_status && std::find(statuses.begin(), statuses.end(), short_status) == statuses.end())
      continue;
    std::ostringstream row;
    row << std::left << std::setw(37) << execution.value("executionId", "")
        << std::setw(30) << (name + "@" + version).substr(0, 29) << std::setw(14)
        << execution.value("runtimeId", "").substr(0, 13) << std::setw(12) << short_status
        << std::setw(15)
        << ActivityName(execution.value("currentActivity", json::object())).substr(0, 14);
    const std::int64_t elapsed_ms =
        DurationMilliseconds(execution.value("totalDuration", json::object()));
    std::string reason = short_status == "QUEUED" ? execution.value("queueReason", "")
                                                   : execution.value("reasonCode", "");
    if (reason.empty() && short_status != "RUNNING") reason = execution.value("message", "");
    row << std::setw(11) << (std::to_string(elapsed_ms) + "ms") << reason << '\n';
    const char* style = short_status == "RUNNING"   ? "\033[32m"
                        : short_status == "QUEUED"  ? "\033[33m"
                        : short_status == "FAILED"  ? "\033[31m"
                                                     : "\033[36m";
    if (options.color)
      table << style << row.str() << "\033[0m";
    else
      table << row.str();
    ++count;
  }
  if (count == 0) table << "(no matching executions)\n";
  return table.str();
}

std::string FormatNotification(const json& params) {
  const std::string uri = params.value("uri", "-");
  return "[update] " + uri +
         (params.contains("sequence") ? " seq=" + Scalar(params.at("sequence")) : "");
}

std::string RenderReply(const std::string& method, const json& reply,
                        const DisplayOptions& options) {
  std::string out;
  const std::string title = ResponseTitle(method, reply);
  if (options.color) {
    AppendColored(kHeading, title, &out);
  } else {
    out = title;
  }
  out += '\n';
  const json& body = reply.is_object() && reply.contains("error") ? reply.at("error") : reply;
  const std::string brief = options.brief && !(reply.is_object() && reply.contains("error"))
                                ? BriefKnown(method, body)
                                : std::string();
  out += brief.empty() ? FormatResult(body, options) : brief;
  return out;
}

}  // namespace eos::ext::repl
