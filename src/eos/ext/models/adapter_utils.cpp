#include "src/eos/ext/models/adapter_utils.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "src/eos/runtime/models/vision_stream.h"
#include "src/eos/lib/log.h"

namespace eos::ext::models::detail {
using runtime::models::Capability;

nlohmann::json ParseJson(std::string_view body) { return nlohmann::json::parse(body); }

std::string EncodeBase64(std::string_view input) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve((input.size() + 2) / 3 * 4);
  for (std::size_t offset = 0; offset < input.size(); offset += 3) {
    const std::uint32_t a = static_cast<unsigned char>(input[offset]);
    const std::uint32_t b =
        offset + 1 < input.size() ? static_cast<unsigned char>(input[offset + 1]) : 0;
    const std::uint32_t c =
        offset + 2 < input.size() ? static_cast<unsigned char>(input[offset + 2]) : 0;
    const std::uint32_t value = (a << 16) | (b << 8) | c;
    output.push_back(kAlphabet[(value >> 18) & 0x3f]);
    output.push_back(kAlphabet[(value >> 12) & 0x3f]);
    output.push_back(offset + 1 < input.size() ? kAlphabet[(value >> 6) & 0x3f] : '=');
    output.push_back(offset + 2 < input.size() ? kAlphabet[value & 0x3f] : '=');
  }
  return output;
}

std::vector<std::uint8_t> DecodeBase64(std::string_view input) {
  static constexpr signed char kInvalid = -1;
  static constexpr signed char kPadding = -2;
  signed char table[256];
  std::fill(std::begin(table), std::end(table), kInvalid);
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (std::size_t i = 0; i < alphabet.size(); ++i) {
    table[static_cast<unsigned char>(alphabet[i])] = static_cast<signed char>(i);
  }
  table[static_cast<unsigned char>('=')] = kPadding;
  if (input.empty() || input.size() % 4 != 0) throw std::invalid_argument("invalid base64");
  std::vector<std::uint8_t> output;
  output.reserve(input.size() / 4 * 3);
  for (std::size_t offset = 0; offset < input.size(); offset += 4) {
    signed char values[4];
    for (int i = 0; i < 4; ++i) {
      values[i] = table[static_cast<unsigned char>(input[offset + i])];
      if (values[i] == kInvalid) throw std::invalid_argument("invalid base64");
    }
    const bool last = offset + 4 == input.size();
    if (values[0] < 0 || values[1] < 0 || (!last && (values[2] < 0 || values[3] < 0)) ||
        (values[2] == kPadding && values[3] != kPadding)) {
      throw std::invalid_argument("invalid base64");
    }
    output.push_back(static_cast<std::uint8_t>((values[0] << 2) | (values[1] >> 4)));
    if (values[2] != kPadding) {
      output.push_back(static_cast<std::uint8_t>(((values[1] & 0x0f) << 4) | (values[2] >> 2)));
      if (values[3] != kPadding) {
        output.push_back(static_cast<std::uint8_t>(((values[2] & 0x03) << 6) | values[3]));
      }
    }
  }
  return output;
}

runtime::models::CallResult Cancelled(const runtime::models::ModelConfig& model) {
  return {runtime::models::CallResult::Status::kUnavailable,
          0,
          {},
          "model '" + model.id + "' request was cancelled"};
}

runtime::models::CallResult StreamException(const runtime::models::ModelConfig& model,
                                            const std::exception& error) {
  using runtime::models::CallResult;
  if (dynamic_cast<const runtime::models::StreamCancelled*>(&error) != nullptr) {
    return Cancelled(model);
  }
  if (dynamic_cast<const runtime::models::StreamTimedOut*>(&error) != nullptr) {
    return {CallResult::Status::kDeadlineExceeded,
            0,
            {},
            "model '" + model.id + "' WebSocket call timed out"};
  }
  if (dynamic_cast<const runtime::models::StreamProtocolError*>(&error) != nullptr) {
    return {CallResult::Status::kProtocolError, 0, {}, "model '" + model.id + "' " + error.what()};
  }
  return {CallResult::Status::kUnavailable,
          0,
          {},
          "model '" + model.id + "' WebSocket failed: " + error.what()};
}

void NormalizeVisionPeople(Capability capability, nlohmann::json* response) {
  if (response == nullptr || !response->is_object() || !response->contains("persons") ||
      !response->at("persons").is_array()) {
    return;
  }
  if (capability == Capability::kDetection) {
    nlohmann::json objects = nlohmann::json::array();
    for (const nlohmann::json& person : response->at("persons")) {
      if (!person.is_object()) continue;
      nlohmann::json object{{"label", "person"}, {"class_id", 0}};
      for (const char* key : {"track_id", "confidence", "bbox"}) {
        if (person.contains(key)) object[key] = person.at(key);
      }
      objects.push_back(std::move(object));
    }
    *response = {{"objects", std::move(objects)}};
    (*response)["object_count"] = response->at("objects").size();
    return;
  }
  if (capability == Capability::kHuman) {
    const auto furniture = response->find("furniture");
    nlohmann::json persons = nlohmann::json::array();
    for (const nlohmann::json& person : response->at("persons")) {
      if (!person.is_object()) continue;
      nlohmann::json normalized = nlohmann::json::object();
      for (const char* key : {"track_id", "confidence", "bbox", "distance_m"}) {
        if (person.contains(key)) normalized[key] = person.at(key);
      }
      persons.push_back(std::move(normalized));
    }
    nlohmann::json normalized{{"persons", std::move(persons)}};
    if (furniture != response->end()) normalized["furniture"] = *furniture;
    *response = std::move(normalized);
    (*response)["person_count"] = response->at("persons").size();
    return;
  }
  if (capability != Capability::kHumanPose && capability != Capability::kPose &&
      capability != Capability::kPosture) return;

  nlohmann::json persons = nlohmann::json::array();
  for (std::size_t person_index = 0; person_index < response->at("persons").size();
       ++person_index) {
    const nlohmann::json& person = response->at("persons").at(person_index);
    if (!person.is_object()) continue;
    if (capability == Capability::kHumanPose && person.contains("pose") &&
        person.at("pose").is_string() && person.contains("posture") &&
        person.at("posture").is_string()) {
      const std::string pose = person.at("pose").get<std::string>();
      const std::string posture = person.at("posture").get<std::string>();
      if ((pose == "stand" && posture != "standing") ||
          (pose == "sit" && posture != "sitting") ||
          (pose == "lie" && posture != "lying") ||
          (pose == "unknown" && posture != "unknown")) {
        throw std::invalid_argument("inconsistent fields 'pose' and 'posture'");
      }
    }
    nlohmann::json compatible = person;
    nlohmann::json normalized = nlohmann::json::object();
    std::vector<const char*> keys{"track_id", "bbox", "confidence", "keypoints",
                                  "keypoint_scores", "distance_m"};
    if (capability == Capability::kPosture)
      keys.insert(keys.end(), {"posture_state", "posture", "posture_confidence",
                               "behavior_event", "behavior_confidence"});
    if (capability == Capability::kHumanPose)
      keys.insert(keys.end(), {"pose", "pose_confidence", "action", "action_confidence"});
    for (const char* key : keys) {
      if (compatible.contains(key) && !compatible.at(key).is_null())
        normalized[key] = compatible.at(key);
    }
    if (capability == Capability::kHumanPose &&
        (!normalized.contains("keypoints") || normalized.at("keypoints").size() != 17 ||
         !normalized.contains("keypoint_scores") ||
         normalized.at("keypoint_scores").size() != 17)) {
      continue;
    }
    if (!normalized.contains("distance_m")) normalized["distance_m"] = nullptr;
    if (normalized.contains("keypoint_scores") && normalized.at("keypoint_scores").is_array()) {
      for (nlohmann::json& score : normalized.at("keypoint_scores")) {
        if (score.is_number()) score = std::clamp(score.get<double>(), 0.0, 1.0);
      }
    }
    persons.push_back(std::move(normalized));
  }
  *response = {{"persons", std::move(persons)}};
  (*response)["person_count"] = response->at("persons").size();
}

void Require(bool condition, const runtime::models::ModelConfig& model,
             const std::string& requirement) {
  if (!condition) {
    eos::lib::LogEvent(eos::lib::GetLogger("models.adapter"), spdlog::level::warn,
                       "model_adapter_rejected",
                       {{"model_id", model.id},
                        {"adapter", model.adapter},
                        {"reason", "incompatible_configuration"},
                        {"requirement", requirement}});
    throw std::invalid_argument("model '" + model.id + "' adapter '" + model.adapter +
                                "' requires " + requirement);
  }
}
}  // namespace eos::ext::models::detail
