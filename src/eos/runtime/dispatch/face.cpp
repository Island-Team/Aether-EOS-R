#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common.h"
#include "dispatcher.h"
#include "src/eos/lib/log.h"
#include "src/eos/runtime/hardware/telemetry_hub.h"
#include "src/eos/runtime/models/runtime.h"

namespace eos::runtime::dispatch {
namespace {

std::string Base64Encode(const std::vector<std::uint8_t>& input) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve((input.size() + 2) / 3 * 4);
  for (std::size_t offset = 0; offset < input.size(); offset += 3) {
    const std::uint32_t first = input[offset];
    const std::uint32_t second = offset + 1 < input.size() ? input[offset + 1] : 0;
    const std::uint32_t third = offset + 2 < input.size() ? input[offset + 2] : 0;
    const std::uint32_t word = (first << 16) | (second << 8) | third;
    output.push_back(kAlphabet[(word >> 18) & 0x3f]);
    output.push_back(kAlphabet[(word >> 12) & 0x3f]);
    output.push_back(offset + 1 < input.size() ? kAlphabet[(word >> 6) & 0x3f] : '=');
    output.push_back(offset + 2 < input.size() ? kAlphabet[word & 0x3f] : '=');
  }
  return output;
}

std::string Trim(std::string value) {
  const auto whitespace = [](unsigned char c) { return std::isspace(c) != 0; };
  value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), whitespace));
  value.erase(std::find_if_not(value.rbegin(), value.rend(), whitespace).base(), value.end());
  return value;
}

bool IsJpeg(const std::vector<std::uint8_t>& jpeg) {
  return jpeg.size() >= 4 && jpeg[0] == 0xff && jpeg[1] == 0xd8 && jpeg[jpeg.size() - 2] == 0xff &&
         jpeg.back() == 0xd9;
}

}  // namespace

nlohmann::json Dispatcher::FaceRegister(const std::string& primitive, const std::string& version,
                                        hardware_runtime::Device& device,
                                        const nlohmann::json& arguments) {
  const std::string name = Trim(arguments.at("name").get<std::string>());
  std::string lowercase = name;
  std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (name.empty() || lowercase == "unknown") {
    return Ack(primitive, version, arguments, false, detail::kInvalidArgument,
               name.empty() ? "name must contain a non-whitespace character"
                            : "name 'unknown' is reserved for unrecognized faces");
  }
  if (models_ == nullptr) {
    return Ack(primitive, version, arguments, false, detail::kUnavailableDependency,
               "face model runtime is unavailable");
  }
  const hardware_runtime::PrimitiveDeclaration* declaration =
      detail::DeclarationOf(device, primitive);
  const models::ModelConfig* model =
      declaration == nullptr ? nullptr : models_->Find(declaration->model_id);
  if (model == nullptr || model->capability != models::Capability::kFace ||
      model->transport != models::Transport::kWebSocket) {
    return Ack(primitive, version, arguments, false, detail::kUnavailableDependency,
               "bound face registration model is unavailable");
  }
  const std::optional<models::ModelHealth> health = models_->Health(model->id);
  if (!health || health->status != models::HealthStatus::kReady) {
    return Ack(primitive, version, arguments, false, detail::kUnavailableDependency,
               "face registration model is not healthy");
  }
  if (!device.ReadHealth().alive) {
    return Ack(primitive, version, arguments, false, detail::kUnavailableDependency,
               "camera is unhealthy");
  }
  const std::optional<hardware::CameraTelemetry> frame =
      telemetry_ ? telemetry_->LatestCamera(device.identity().device_id)
                 : device.feeds().ReadCameraTelemetry();
  constexpr std::size_t kDefaultMaximumFrameBytes = 4U * 1024U * 1024U;
  const std::size_t maximum =
      device.part().ros2 ? device.part().ros2->max_image_bytes : kDefaultMaximumFrameBytes;
  if (!frame || frame->width_px == 0 || frame->height_px == 0 || frame->frame_id.empty() ||
      frame->jpeg.size() > maximum || !IsJpeg(frame->jpeg)) {
    return Ack(primitive, version, arguments, false, detail::kUnavailableDependency,
               "camera has no valid current frame");
  }
  lib::LogEvent(lib::GetLogger("dispatch.face"), spdlog::level::info,
                "face_registration_requested",
                {{"model_id", model->id},
                 {"device_id", device.identity().device_id},
                 {"width_px", frame->width_px},
                 {"height_px", frame->height_px},
                 {"size_bytes", frame->jpeg.size()}});
  const models::CallResult result = models_->RegisterFace(
      model->id,
      {{"data_b64", Base64Encode(frame->jpeg)},
       {"filename", "camera.jpg"},
       {"media_type", "image/jpeg"}},
      name);
  if (result.status != models::CallResult::Status::kCompleted) {
    return Ack(primitive, version, arguments, false,
               result.status == models::CallResult::Status::kInvalidArgument
                   ? detail::kInvalidArgument
                   : detail::kUnavailableDependency,
               "face registration failed: " + result.message);
  }
  if (!result.response.is_object() || !result.response.contains("data") ||
      !result.response.at("data").is_object() || !result.response.at("data").contains("name") ||
      !result.response.at("data").at("name").is_string()) {
    return Ack(primitive, version, arguments, false, detail::kUnavailableDependency,
               "face registration returned an invalid response");
  }
  nlohmann::json ack = Accepted(primitive, version, arguments);
  ack["result"] = {{"status", "succeeded"},
                   {"name", result.response.at("data").at("name")},
                   {"deviceId", device.identity().device_id}};
  return ack;
}

}  // namespace eos::runtime::dispatch
