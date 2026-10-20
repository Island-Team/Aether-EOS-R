#include "src/eos/runtime/init/runtime.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "eos/internal/northbound/server.h"
#include "share/skills/native/skills.h"
#include "src/eos/ext/models/external_adapter.h"
#include "src/eos/lib/clock.h"
#include "src/eos/lib/log.h"
#include "src/eos/lib/paths.h"
#include "src/eos/lib/timer.h"
#include "src/eos/lib/uuid.h"
#include "src/eos/runtime/actionlist/manager.h"
#include "src/eos/runtime/dispatch/dispatcher.h"
#include "src/eos/runtime/hardware/registry.h"
#include "src/eos/runtime/hardware/telemetry_hub.h"
#include "src/eos/runtime/init/config.h"
#include "src/eos/runtime/init/prelaunch.h"
#include "src/eos/runtime/init/rotated_frame.h"
#include "src/eos/runtime/init/schemas.h"
#include "src/eos/runtime/models/camera_object.h"
#include "src/eos/runtime/models/camera_pose.h"
#include "src/eos/runtime/models/microphone_vad.h"
#include "src/eos/runtime/models/runtime.h"
#include "src/eos/runtime/northbound/service.h"
#include "src/eos/runtime/perception/body/detection_overlay.h"
#include "src/eos/runtime/perception/body/reader.h"
#include "src/eos/runtime/perception/producer_coordinator.h"
#include "src/eos/runtime/perception/product_history.h"
#include "src/eos/runtime/perception/resource_catalog.h"
#include "src/eos/runtime/skills/builtin/registry.h"
#include "src/eos/runtime/skills/manager/manager.h"
#include "src/eos/runtime/skills/native/runtime.h"
#include "src/eos/runtime/store/navigation_point_store.h"
#include "src/eos/runtime/store/furniture_region_store.h"

namespace eos::runtime::init {
namespace {

std::optional<std::int64_t> LatestGraspFrameTimestamp() {
  namespace beast = boost::beast;
  namespace http = beast::http;
  namespace asio = boost::asio;
  try {
    asio::io_context context;
    beast::tcp_stream stream(context);
    stream.expires_after(std::chrono::milliseconds(200));
    stream.connect({asio::ip::make_address("127.0.0.1"), 18767});
    http::request<http::empty_body> request{http::verb::get, "/health", 11};
    request.set(http::field::host, "127.0.0.1");
    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    if (response.result() != http::status::ok) return std::nullopt;
    const auto status = nlohmann::json::parse(response.body());
    if (status.value("status", "") != "ready" ||
        !status.contains("latest_source_timestamp_ns") ||
        !status.at("latest_source_timestamp_ns").is_string())
      return std::nullopt;
    const auto text = status.at("latest_source_timestamp_ns").get<std::string>();
    std::int64_t timestamp = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), timestamp);
    return error == std::errc{} && end == text.data() + text.size() && timestamp > 0
               ? std::optional<std::int64_t>(timestamp)
               : std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

void LogModuleStatus(const std::shared_ptr<spdlog::logger>& logger, const char* module,
                     const char* status, nlohmann::json details = nlohmann::json::object()) {
  details["module"] = module;
  details["status"] = status;
  lib::LogEvent(logger, spdlog::level::info, "runtime_module_status", details);
}

models::Capability ModelCapability(const std::string& capability) {
  if (capability == "audio.synthesis") return models::Capability::kTts;
  if (capability == "audio.transcription") return models::Capability::kAsr;
  if (capability == "audio.vad") return models::Capability::kVad;
  if (capability == "vision.object_detection") return models::Capability::kDetection;
  if (capability == "vision.pose") return models::Capability::kPose;
  if (capability == "vision.posture") return models::Capability::kPosture;
  if (capability == "vision.face") return models::Capability::kFace;
  throw std::logic_error("unknown advanced-primitive model capability: " + capability);
}

std::string Base64Encode(const std::vector<std::uint8_t>& input) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve((input.size() + 2) / 3 * 4);
  for (std::size_t offset = 0; offset < input.size(); offset += 3) {
    const std::uint32_t a = input[offset];
    const std::uint32_t b = offset + 1 < input.size() ? input[offset + 1] : 0;
    const std::uint32_t c = offset + 2 < input.size() ? input[offset + 2] : 0;
    const std::uint32_t value = (a << 16) | (b << 8) | c;
    output.push_back(kAlphabet[(value >> 18) & 0x3f]);
    output.push_back(kAlphabet[(value >> 12) & 0x3f]);
    output.push_back(offset + 1 < input.size() ? kAlphabet[(value >> 6) & 0x3f] : '=');
    output.push_back(offset + 2 < input.size() ? kAlphabet[value & 0x3f] : '=');
  }
  return output;
}

std::uint64_t TrackId(const nlohmann::json& options) {
  if (options.contains("track_id") && options.at("track_id").is_number_unsigned()) {
    const auto track_id = options.at("track_id").get<std::uint64_t>();
    if (track_id != 0) return track_id;
  }
  if (options.contains("human_id") && options.at("human_id").is_string()) {
    const std::string human_id = options.at("human_id").get<std::string>();
    const std::size_t separator = human_id.rfind(':');
    const std::string_view suffix(
        human_id.data() + (separator == std::string::npos ? 0 : separator + 1),
        human_id.size() - (separator == std::string::npos ? 0 : separator + 1));
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(suffix.data(), suffix.data() + suffix.size(), parsed);
    if (result.ec == std::errc{} && result.ptr == suffix.data() + suffix.size() && parsed != 0) {
      return parsed;
    }
  }
  return 1;
}

void RequireModel(const models::Runtime& runtime, const std::string& id,
                  models::Capability capability) {
  const std::string prefix = "perception model '" + id + "'";
  switch (runtime.Resolve(id, capability)) {
    case models::ModelReferenceState::kEnabled:
      return;
    case models::ModelReferenceState::kDisabled:
      throw std::runtime_error(prefix + " is disabled");
    case models::ModelReferenceState::kMissing:
      throw std::runtime_error(prefix + " is not configured");
    case models::ModelReferenceState::kWrongCapability:
      throw std::runtime_error(prefix + " must have capability " +
                               std::string(models::CapabilityName(capability)));
  }
}

/// Advanced primitives are part of the launched command surface, so their
/// exact model dependencies must be configured and enabled at the startup
/// barrier. Provider health may still change later and is handled per call.
}  // namespace

void ValidateAdvancedPrimitiveModels(const hardware_runtime::Binding& binding,
                                     const models::Runtime& models) {
  for (const hardware_runtime::DeviceBinding& device : binding.devices) {
    for (const hardware_runtime::PrimitiveDeclaration& primitive : device.part.primitives) {
      const hardware_runtime::PrimitiveSpec* spec =
          hardware_runtime::PrimitiveSpecOf(primitive.name, primitive.version);
      if (spec == nullptr || spec->kind != hardware_runtime::PrimitiveKind::kAdvanced) continue;
      const models::Capability expected = ModelCapability(spec->model_capability);
      const std::string prefix = "advanced primitive '" + primitive.name + "@" + primitive.version +
                                 "' on " + device.part.kind + " part '" + device.part.part_id +
                                 "' model '" + primitive.model_id + "'";
      switch (models.Resolve(primitive.model_id, expected)) {
        case models::ModelReferenceState::kEnabled:
          if (primitive.name == "face.register" &&
              models.Find(primitive.model_id)->transport != models::Transport::kWebSocket) {
            throw std::runtime_error(prefix + " must use websocket transport");
          }
          lib::LogEvent(lib::GetLogger("hardware"), spdlog::level::info,
                        "advanced_primitive_model_admitted",
                        {{"primitive", primitive.name},
                         {"primitive_version", primitive.version},
                         {"device_class", device.part.kind},
                         {"part_id", device.part.part_id},
                         {"model_id", primitive.model_id},
                         {"capability", spec->model_capability}});
          break;
        case models::ModelReferenceState::kDisabled:
          throw std::runtime_error(prefix + " is disabled; set enabled = true to launch it");
        case models::ModelReferenceState::kMissing:
          throw std::runtime_error(prefix + " is not configured");
        case models::ModelReferenceState::kWrongCapability:
          throw std::runtime_error(prefix + " must have capability " + spec->model_capability);
      }
    }
  }
}

bool ValidateCameraObjectModel(const models::Runtime& models, const std::string& model_id) {
  if (model_id.empty()) return false;
  RequireModel(models, model_id, models::Capability::kDetection);
  return true;
}

std::string ResolveSkillChatModel(const models::Runtime& models) {
  const std::string kModelId = "gemma4-26b-chat";
  return models.Resolve(kModelId, models::Capability::kChatCompletion) ==
                 models::ModelReferenceState::kEnabled
             ? std::string(kModelId)
             : std::string{};
}

std::string ResolveSkillVlmModel(const models::Runtime& models) {
  const std::string kModelId = "vla-gemma4";
  return models.Resolve(kModelId, models::Capability::kVlm) ==
                 models::ModelReferenceState::kEnabled
             ? std::string(kModelId)
             : std::string{};
}

skills::InferenceResult RunSkillInference(models::Runtime& models,
                                          const models::CameraPoseStore& camera_pose,
                                          const InferenceModelBindings& bindings,
                                          const skills::InferenceRequest& inference,
                                          skills::Cancelled cancelled) {
  if (!inference.options.is_object()) {
    throw std::invalid_argument("inference options must be an object");
  }
  const bool has_any_provenance = !inference.source.empty() || inference.source_sequence != 0 ||
                                  inference.source_generation != 0;
  const bool has_exact_provenance = !inference.source.empty() && inference.source_sequence != 0 &&
                                    inference.source_generation != 0;
  const bool has_inline_media =
      inference.options.contains("input") || inference.options.contains("depth_input");

  if (inference.options.contains("rotation_degrees")) {
    if (!has_exact_provenance || has_inline_media ||
        (inference.authority != skills::InferenceAuthority::kHumanPose &&
         inference.authority != skills::InferenceAuthority::kFaceRecognition)) {
      throw std::invalid_argument(
          "rotated inference requires a pose or face request for one exact retained frame");
    }
    std::vector<int> rotations;
    const auto& option = inference.options.at("rotation_degrees");
    if (inference.authority == skills::InferenceAuthority::kHumanPose) {
      if (option == nlohmann::json::array({-90, 90})) {
        rotations = {-90, 90};
      } else if (option.is_number_integer() &&
                 (option.get<int>() == -90 || option.get<int>() == 90)) {
        rotations = {option.get<int>()};
      } else {
        throw std::invalid_argument("pose rotation_degrees must be [-90, 90], -90, or 90");
      }
      for (auto it = inference.options.begin(); it != inference.options.end(); ++it) {
        if (it.key() != "rotation_degrees" && it.key() != "conf" && it.key() != "kpt_conf")
          throw std::invalid_argument("unsupported rotated pose option: " + it.key());
      }
    } else {
      if (!option.is_number_integer() || (option.get<int>() != -90 && option.get<int>() != 90))
        throw std::invalid_argument("face rotation_degrees must be -90 or 90");
      rotations = {option.get<int>()};
      for (auto it = inference.options.begin(); it != inference.options.end(); ++it) {
        if (it.key() != "rotation_degrees" && it.key() != "return_image")
          throw std::invalid_argument("unsupported rotated face option: " + it.key());
      }
    }
    const auto snapshot = camera_pose.Find(inference.source, inference.source_sequence,
                                           inference.source_generation);
    if (!snapshot || !snapshot->jpeg || snapshot->jpeg->empty() ||
        snapshot->image_width_px == 0 || snapshot->image_height_px == 0)
      throw std::invalid_argument("exact retained camera frame is unavailable");
    struct FollowupRelease {
      const models::CameraPoseStore& store;
      const skills::InferenceRequest& request;
      bool enabled;
      ~FollowupRelease() {
        if (enabled)
          store.ReleaseExactFollowup(request.source, request.source_sequence,
                                     request.source_generation);
      }
    } release{camera_pose, inference,
              inference.authority == skills::InferenceAuthority::kFaceRecognition};
    std::vector<std::uint16_t> decoded_depth;
    const std::vector<std::uint16_t>* source_depth = nullptr;
    if (inference.authority == skills::InferenceAuthority::kHumanPose && snapshot->depth_u16 &&
        snapshot->depth_u16->size() == static_cast<std::size_t>(snapshot->image_width_px) *
                                         snapshot->image_height_px) {
      source_depth = snapshot->depth_u16.get();
    } else if (inference.authority == skills::InferenceAuthority::kHumanPose &&
               snapshot->depth_png && !snapshot->depth_png->empty()) {
      decoded_depth = DecodeAlignedDepthPng(*snapshot->depth_png, snapshot->image_width_px,
                                            snapshot->image_height_px);
      source_depth = &decoded_depth;
    }
    const std::string model_id = inference.authority == skills::InferenceAuthority::kHumanPose
                                     ? bindings.human_pose
                                     : bindings.face;
    if (model_id.empty()) throw std::runtime_error("requested inference role is not configured");
    nlohmann::json results = nlohmann::json::array();
    for (const int degrees : rotations) {
      if (cancelled && cancelled()) throw std::runtime_error("rotated inference cancelled");
      const auto rotated_jpeg = RotateJpegQuarterTurn(*snapshot->jpeg, degrees);
      nlohmann::json rotated_request = {
          {"input", {{"data_b64", Base64Encode(rotated_jpeg)},
                     {"filename", "camera-rotated.jpg"}, {"media_type", "image/jpeg"}}}};
      if (inference.authority == skills::InferenceAuthority::kHumanPose) {
        for (const char* key : {"conf", "kpt_conf"})
          if (inference.options.contains(key)) rotated_request[key] = inference.options.at(key);
        if (source_depth) {
          const auto rotated_depth = RotateDepthQuarterTurn(
              *source_depth, snapshot->image_width_px, snapshot->image_height_px, degrees);
          const auto depth_png = models::EncodeAlignedDepthPng(
              rotated_depth, snapshot->image_height_px, snapshot->image_width_px);
          if (depth_png.empty()) throw std::runtime_error("failed to encode rotated depth");
          rotated_request["depth_input"] = {{"data_b64", Base64Encode(depth_png)},
                                              {"filename", "depth-rotated.png"},
                                              {"media_type", "image/png"}};
          rotated_request["depth_scale"] = snapshot->depth_scale;
          if (snapshot->depth_intrinsics &&
              snapshot->depth_intrinsics->width_px == snapshot->image_width_px &&
              snapshot->depth_intrinsics->height_px == snapshot->image_height_px) {
            rotated_request["depth_intrinsics"] =
                RotateDepthIntrinsics(*snapshot->depth_intrinsics, degrees);
          }
        }
      } else {
        rotated_request["operation"] = "recognize";
        if (inference.options.contains("return_image"))
          rotated_request["return_image"] = inference.options.at("return_image");
      }
      models::CallResult called = models.Call(model_id, rotated_request, cancelled);
      if (called.status != models::CallResult::Status::kCompleted)
        throw std::runtime_error("model '" + model_id + "' inference failed: " +
                                 (called.message.empty() ? "unknown model error" : called.message));
      if (inference.authority == skills::InferenceAuthority::kFaceRecognition)
        return {inference.authority, inference.source, inference.source_sequence,
                inference.source_generation, std::move(called.response)};
      if (!called.response.is_object() || !called.response.contains("persons") ||
          !called.response.at("persons").is_array())
        throw std::runtime_error("rotated pose model returned invalid persons");
      for (auto& person : called.response["persons"])
        MapRotatedPersonToSource(person, snapshot->image_width_px,
                                 snapshot->image_height_px, degrees);
      if (source_depth) {
        hardware::CameraTelemetry aligned_depth;
        aligned_depth.width_px = snapshot->image_width_px;
        aligned_depth.height_px = snapshot->image_height_px;
        aligned_depth.depth_u16 = *source_depth;
        aligned_depth.depth_scale = snapshot->depth_scale;
        models::AddAlignedKeypointDepths(called.response, aligned_depth);
      }
      results.push_back({{"degrees", degrees}, {"persons", called.response.at("persons")},
                         {"person_count", called.response.at("persons").size()}});
    }
    camera_pose.RetainExactForFollowup(*snapshot);
    return {inference.authority, inference.source, inference.source_sequence,
            inference.source_generation, {{"rotations", std::move(results)}}};
  }

  nlohmann::json request;
  std::string model_id;
  if (inference.authority == skills::InferenceAuthority::kChatCompletion) {
    if (has_any_provenance || has_inline_media) {
      throw std::invalid_argument(
          "chat completion requires text options and empty source provenance");
    }
    model_id = bindings.chat;
    request = inference.options;
  } else if (inference.authority == skills::InferenceAuthority::kVlm) {
    if (inference.options.contains("depth_input") ||
        (has_any_provenance && (!has_exact_provenance || has_inline_media))) {
      throw std::invalid_argument(
          "VLM inference requires text with optional inline image or one exact retained frame");
    }
    model_id = bindings.vlm;
    request = inference.options;
    if (has_exact_provenance) {
      const auto snapshot = camera_pose.Find(inference.source, inference.source_sequence,
                                             inference.source_generation);
      if (!snapshot || !snapshot->jpeg || snapshot->jpeg->empty())
        throw std::invalid_argument("exact retained camera frame is unavailable");
      request["input"] = {{"data_b64", Base64Encode(*snapshot->jpeg)},
                          {"media_type", "image/jpeg"}};
    }
  } else if (inference.authority == skills::InferenceAuthority::kVad) {
    if (has_any_provenance || !inference.options.contains("input")) {
      throw std::invalid_argument(
          "VAD inference requires canonical input and empty source provenance");
    }
    model_id = bindings.vad;
    request = inference.options;
  } else {
    if (has_any_provenance == has_inline_media || (has_any_provenance && !has_exact_provenance)) {
      throw std::invalid_argument(
          "vision inference requires either one exact retained frame or inline media");
    }
    if (has_inline_media) {
      request = inference.options;
    } else {
      const auto snapshot = camera_pose.Find(inference.source, inference.source_sequence,
                                             inference.source_generation);
      if (!snapshot || !snapshot->jpeg || snapshot->jpeg->empty()) {
        const auto latest = camera_pose.Latest();
        lib::LogEvent(
            lib::GetLogger("skills.inference"), spdlog::level::err,
            "retained_camera_frame_unavailable",
            {{"authority", static_cast<int>(inference.authority)},
             {"source", inference.source},
             {"source_sequence", inference.source_sequence},
             {"source_generation", inference.source_generation},
             {"snapshot_found", snapshot.has_value()},
             {"snapshot_has_jpeg", snapshot && snapshot->jpeg && !snapshot->jpeg->empty()},
             {"latest_source", latest ? latest->device_id : std::string{}},
             {"latest_sequence", latest ? latest->sequence : 0},
             {"latest_generation", latest ? latest->generation : 0}});
        throw std::invalid_argument("exact retained camera frame is unavailable");
      }
      lib::LogEvent(lib::GetLogger("skills.inference"), spdlog::level::info,
                    "stable_frame_inference_started",
                    {{"authority", static_cast<int>(inference.authority)},
                     {"source", inference.source},
                     {"source_sequence", inference.source_sequence},
                     {"source_generation", inference.source_generation},
                     {"input_bytes", snapshot->jpeg->size()}});
      request["input"] = {{"data_b64", Base64Encode(*snapshot->jpeg)},
                          {"filename", "camera.jpg"},
                          {"media_type", "image/jpeg"}};
      if (snapshot->depth_png && !snapshot->depth_png->empty()) {
        request["depth_input"] = {{"data_b64", Base64Encode(*snapshot->depth_png)},
                                  {"filename", "depth.png"},
                                  {"media_type", "image/png"}};
        request["depth_scale"] = snapshot->depth_scale;
      } else if (snapshot->depth_u16 && !snapshot->depth_u16->empty()) {
        const auto encoded = models::EncodeAlignedDepthPng(
            *snapshot->depth_u16, snapshot->image_width_px, snapshot->image_height_px);
        if (!encoded.empty()) {
          request["depth_input"] = {{"data_b64", Base64Encode(encoded)},
                                    {"filename", "depth.png"},
                                    {"media_type", "image/png"}};
          request["depth_scale"] = snapshot->depth_scale;
        }
      }
    }

    switch (inference.authority) {
      case skills::InferenceAuthority::kHuman:
        model_id = bindings.human;
        if (!has_inline_media && inference.options.contains("conf")) {
          request["conf"] = inference.options.at("conf");
        }
        break;
      case skills::InferenceAuthority::kHumanPose:
        model_id = bindings.human_pose;
        if (!has_inline_media) {
          const auto bbox = inference.options.find("bbox");
          if (bbox == inference.options.end() || !bbox->is_array() || bbox->size() != 4) {
            throw std::invalid_argument("human-pose inference requires a four-value bbox");
          }
          request["persons"] = {{{"track_id", TrackId(inference.options)},
                                 {"bbox", *bbox},
                                 {"confidence", inference.options.value("confidence", 1.0)}}};
          for (const char* key : {"conf", "kpt_conf"}) {
            if (inference.options.contains(key)) request[key] = inference.options.at(key);
          }
        }
        break;
      case skills::InferenceAuthority::kFaceRecognition:
        model_id = bindings.face;
        request["operation"] = "recognize";
        if (!has_inline_media && inference.options.contains("return_image")) {
          request["return_image"] = inference.options.at("return_image");
        }
        if (!has_inline_media) {
          request.erase("depth_input");
          request.erase("depth_scale");
        }
        break;
      case skills::InferenceAuthority::kVad:
      case skills::InferenceAuthority::kChatCompletion:
      case skills::InferenceAuthority::kVlm:
        break;
    }
  }

  if (model_id.empty()) throw std::runtime_error("requested inference role is not configured");
  models::CallResult called = models.Call(model_id, request, std::move(cancelled));
  if (called.status != models::CallResult::Status::kCompleted) {
    throw std::runtime_error("model '" + model_id + "' inference failed: " +
                             (called.message.empty() ? "unknown model error" : called.message));
  }
  if (has_exact_provenance) {
    lib::LogEvent(lib::GetLogger("skills.inference"), spdlog::level::info,
                  "stable_frame_inference_completed",
                  {{"authority", static_cast<int>(inference.authority)},
                   {"source", inference.source},
                   {"source_sequence", inference.source_sequence},
                   {"source_generation", inference.source_generation},
                   {"model_id", model_id}});
  }
  return {inference.authority, inference.source, inference.source_sequence,
          inference.source_generation, std::move(called.response)};
}

int Run(Options options, StopRequested stop_requested) {
  const lib::LayoutPaths paths = lib::ResolveLayoutPaths(options.etc, options.contracts);
  const std::filesystem::path config_path =
      options.config.empty() ? paths.etc / "eos.toml" : std::filesystem::path(options.config);
  if (!config_path.is_absolute()) {
    throw lib::ConfigRootError("config path must be absolute: " + config_path.string());
  }
  const Config config = LoadConfig(config_path.string());
  if (options.host.empty()) options.host = config.host;
  if (options.grpc_port == 0) options.grpc_port = config.grpc_port;
  if (options.grpc_port == 16004) {
    throw std::runtime_error("port 16004 is reserved for the external EOS simulator service");
  }

  lib::LogConfig log_config = config.log;
  if (!options.log_level.empty()) log_config.console_level = options.log_level;
  lib::ConfigureLogging(paths.log, "eosd", log_config);
  const std::filesystem::path process_log_dir =
      paths.log / "runs" / lib::CurrentLogProcessContext().launch_id / "eosd";
  const std::filesystem::path run_log_dir = process_log_dir.parent_path();
  if (config.log.overwrite && !config.log.save_attachments) {
    std::error_code attachment_error;
    std::filesystem::remove_all(run_log_dir / "attachments", attachment_error);
    if (attachment_error) {
      throw std::runtime_error("cannot clear run attachments: " + attachment_error.message());
    }
  }
  lib::ConfigureLogAttachments(
      config.log.save_attachments ? run_log_dir / "attachments" : std::filesystem::path{},
      config.log.overwrite);
  auto log = lib::GetLogger("eosd");
  LogModuleStatus(log, "configuration", "ready",
                  {{"path", config_path.string()},
                   {"etc_root", paths.etc.string()},
                   {"contracts_root", paths.contracts.string()},
                   {"bindings", config.mode.bindings.size()},
                   {"models", config.models.size()},
                   {"models_enabled", std::count_if(config.models.begin(), config.models.end(),
                                                    [](const models::ModelConfig& model) {
                                                      return model.enabled;
                                                    })}});

  PrelaunchProcesses prelaunch;
  if (options.no_prelaunch) {
    LogModuleStatus(log, "prelaunch", "disabled", {{"reason", "command_line"}});
  } else if (!config.prelaunch.empty()) {
    prelaunch = RunPrelaunch(config.prelaunch, process_log_dir.string());
    LogModuleStatus(log, "prelaunch", "ready", {{"commands", config.prelaunch.size()}});
  } else {
    LogModuleStatus(log, "prelaunch", "disabled", {{"reason", "not_configured"}});
  }

  PrelaunchProcesses managed_adapter;
  if (options.no_prelaunch || config.adapter.shell.empty()) {
    LogModuleStatus(log, "adapter_service", "disabled",
                    {{"reason", options.no_prelaunch ? "command_line" : "not_configured"}});
  } else {
    managed_adapter = RunManagedService(config.adapter, process_log_dir.string());
    LogModuleStatus(log, "adapter_service", "ready");
  }

  const SchemaStore schemas(paths.contracts);
  LogModuleStatus(log, "contracts", "ready",
                  {{"primitives", schemas.PrimitiveIdentities().size()},
                   {"resource_types", schemas.ResourceTypes().size()}});
  models::Runtime models(config.models, {}, eos::ext::models::CreateAdapter);
  bool camera_pose_available = false;
  bool camera_pose_enrichment_available = false;
  std::string camera_face_model_id;
  const bool camera_object_available =
      ValidateCameraObjectModel(models, config.camera_object.model_id);
  bool microphone_vad_available = false;
  bool microphone_asr_available = false;
  const bool camera_stream_requested = config.perception_stream.human ||
                                       config.perception_stream.human_pose ||
                                       config.perception_stream.face;
  if (config.perception_stream.vad) {
    if (config.microphone_vad.model_id.empty()) {
      throw std::runtime_error("vad stream requires [perception.microphone_vad]");
    }
    RequireModel(models, config.microphone_vad.model_id, models::Capability::kVad);
  }
  if (camera_stream_requested) {
    if (config.camera_pose.rate_hz == 0 || config.camera_pose.detection_model_id.empty()) {
      throw std::runtime_error(
          "human stream requires [perception.camera_pose] detection_model_id and rate_hz");
    }
    RequireModel(models, config.camera_pose.detection_model_id, models::Capability::kHuman);
  }
  if (config.perception_stream.human_pose) {
    RequireModel(models, config.camera_pose.model_id, models::Capability::kHumanPose);
  }
  if (!config.camera_pose.posture_model_id.empty()) {
    RequireModel(models, config.camera_pose.posture_model_id,
                 models::Capability::kPostureClassification);
  }
  if (config.perception_stream.face) {
    if (config.camera_face.model_id.empty()) {
      throw std::runtime_error("face stream requires [perception.camera_face] model_id");
    }
    RequireModel(models, config.camera_face.model_id, models::Capability::kFace);
  }
  camera_pose_available =
      config.camera_pose.rate_hz != 0 && !config.camera_pose.detection_model_id.empty() &&
      models.Resolve(config.camera_pose.detection_model_id, models::Capability::kHuman) ==
          models::ModelReferenceState::kEnabled;
  if (camera_stream_requested && !camera_pose_available) {
    throw std::runtime_error(
        "human, human_pose, and face streams require an available camera human model");
  }
  camera_pose_enrichment_available =
      !config.camera_pose.model_id.empty() &&
      models.Resolve(config.camera_pose.model_id, models::Capability::kHumanPose) ==
          models::ModelReferenceState::kEnabled;
  const bool camera_posture_available =
      !config.camera_pose.posture_model_id.empty() && camera_pose_available &&
      models.Resolve(config.camera_pose.posture_model_id, models::Capability::kPostureClassification) ==
          models::ModelReferenceState::kEnabled;
  if (!config.camera_face.model_id.empty() &&
      models.Resolve(config.camera_face.model_id, models::Capability::kFace) ==
          models::ModelReferenceState::kEnabled) {
    camera_face_model_id = config.camera_face.model_id;
  }
  LogModuleStatus(log, "camera_pose", camera_pose_available ? "ready" : "disabled",
                  camera_pose_available
                      ? nlohmann::json{{"model_id", config.camera_pose.detection_model_id},
                                       {"rate_hz", config.camera_pose.rate_hz}}
                      : nlohmann::json{{"reason", "stream_disabled"}});
  LogModuleStatus(log, "camera_face", camera_face_model_id.empty() ? "disabled" : "ready",
                  camera_face_model_id.empty()
                      ? nlohmann::json{{"reason", "stream_disabled"}}
                      : nlohmann::json{{"model_id", camera_face_model_id}});
  LogModuleStatus(log, "camera_object", camera_object_available ? "ready" : "disabled",
                  camera_object_available
                      ? nlohmann::json{{"model_id", config.camera_object.model_id},
                                       {"rate_hz", config.camera_object.rate_hz}}
                      : nlohmann::json{{"reason", "not_configured"}});
  if (config.perception_stream.vad) {
    switch (models.Resolve(config.microphone_vad.model_id, models::Capability::kVad)) {
      case models::ModelReferenceState::kEnabled:
        microphone_vad_available = true;
        LogModuleStatus(log, "microphone_vad", "ready",
                        {{"model_id", config.microphone_vad.model_id},
                         {"asr_model_id", config.microphone_vad.asr_model_id},
                         {"window_ms", config.microphone_vad.window_ms}});
        break;
      case models::ModelReferenceState::kDisabled:
        LogModuleStatus(
            log, "microphone_vad", "disabled",
            {{"model_id", config.microphone_vad.model_id}, {"reason", "model_disabled"}});
        break;
      case models::ModelReferenceState::kMissing:
        throw std::runtime_error("microphone VAD model is not configured: " +
                                 config.microphone_vad.model_id);
      case models::ModelReferenceState::kWrongCapability:
        throw std::runtime_error("microphone VAD model '" + config.microphone_vad.model_id +
                                 "' must have capability audio.vad");
    }
  } else {
    LogModuleStatus(log, "microphone_vad", "disabled", {{"reason", "not_configured"}});
  }
  if (!config.microphone_vad.model_id.empty() &&
      models.Resolve(config.microphone_vad.model_id, models::Capability::kVad) ==
          models::ModelReferenceState::kEnabled) {
    microphone_vad_available = true;
  }
  if (!config.microphone_vad.asr_model_id.empty() &&
      models.Resolve(config.microphone_vad.asr_model_id, models::Capability::kAsr) ==
          models::ModelReferenceState::kEnabled) {
    microphone_asr_available = true;
  }
  lib::SystemClock clock;
  hardware_runtime::Registry registry = [&] {
    try {
      const hardware_runtime::ModeBinding& binding = config.mode.bindings.front();
      hardware_runtime::Binding parsed =
          hardware_runtime::LoadBinding((paths.etc / binding.backend).string(), binding.profile);
      ValidateAdvancedPrimitiveModels(parsed, models);
      return hardware_runtime::Registry::Adopt(std::move(parsed), "", paths.run / "simulator.sock");
    } catch (const std::exception& error) {
      lib::LogEvent(log, spdlog::level::err, "runtime_binding_failed", {{"reason", error.what()}});
      throw std::runtime_error("runtime binding: " + std::string(error.what()));
    }
  }();
  hardware_runtime::TelemetryHub telemetry(registry, clock);
  std::size_t alive_devices = 0;
  std::size_t admitted_devices = 0;
  for (const hardware_runtime::Device& device : registry.devices()) {
    const hardware_runtime::DeviceHealth health = device.ReadHealth();
    if (health.alive) ++alive_devices;
    if (health.admitted) ++admitted_devices;
  }
  LogModuleStatus(log, "hardware", "ready",
                  {{"profile", config.mode.bindings.front().profile},
                   {"robot_id", registry.robot().id},
                   {"devices", registry.devices().size()},
                   {"alive", alive_devices},
                   {"admitted", admitted_devices}});
  models::CameraPoseStore camera_pose;
  models::CameraObjectStore camera_object;
  models::MicrophoneVadStore microphone_vad;
  perception::body::DetectionOverlayStore detection_overlays;
  store::FurnitureRegionStore furniture_regions(paths.state / "custom-store");
  const std::string default_object_camera_device_id =
      camera_object_available
          ? registry.Find(hardware::DeviceKind::kCamera, config.camera_object.camera_part_id)
                .identity().device_id
          : std::string{};
  perception::body::Reader body(registry, clock, &camera_pose, &camera_object, &microphone_vad,
                                 &detection_overlays, &furniture_regions,
                                 default_object_camera_device_id);
  store::NavigationPointStore navigation_points(paths.state / "custom-store");
  perception::ResourceCatalog resources(&body);
  resources.RegisterProvider(
      {[&navigation_points] { return navigation_points.ListResources(); },
       [&navigation_points](const std::string& uri) { return navigation_points.ReadResource(uri); },
       [&navigation_points](const std::string& uri_template) {
         return navigation_points.SupportsResourceTemplate(uri_template);
       }});
  LogModuleStatus(log, "navigation_point_store", "ready",
                   {{"path", navigation_points.path().string()}});
  LogModuleStatus(log, "furniture_region_store", "ready",
                  {{"path", furniture_regions.path().string()}});
  const std::string robot_name =
      registry.robots().empty() ? registry.robot().name : registry.robots().front().name;
  const std::string boot_id = lib::MintUuidV7();
  std::unique_ptr<perception::ProductHistory> detection_history;
  std::unique_ptr<perception::ProductHistory> head_detection_history;
  std::map<std::string, std::unique_ptr<perception::ProductHistory>> camera_detection_histories;
  std::unique_ptr<perception::ProductHistory> vad_history;
  std::unique_ptr<perception::ProductHistory> asr_history;
  std::shared_ptr<perception::ProductHistoryWriter> detection_writer;
  std::shared_ptr<perception::ProductHistoryWriter> head_detection_writer;
  std::map<std::string, std::shared_ptr<perception::ProductHistoryWriter>> camera_detection_writers;
  std::shared_ptr<perception::ProductHistoryWriter> vad_writer;
  std::shared_ptr<perception::ProductHistoryWriter> asr_writer;
  if (camera_pose_available) {
    const std::string schema_id = "egia.contracts.perception.world.detection.v1";
    detection_history = std::make_unique<perception::ProductHistory>(
        perception::ProductDeclaration{"robots/" + robot_name + "/detection/state", schema_id,
                                       boot_id, boot_id,
                                       [&schemas, schema_id](const nlohmann::json& value) {
                                         schemas.ValidateById(schema_id, value);
                                       }},
        128);
    detection_writer =
        std::make_shared<perception::ProductHistoryWriter>(detection_history->TakeWriter());
  }
  if (camera_object_available) {
    const std::string schema_id = "egia.contracts.perception.world.detection.v1";
    for (const auto& device : registry.devices()) {
      if (device.identity().kind != hardware::DeviceKind::kCamera) continue;
      const std::string device_id = device.identity().device_id;
      auto history = std::make_unique<perception::ProductHistory>(
          perception::ProductDeclaration{"robots/" + robot_name + "/cameras/" + device_id +
                                             "/detections/state", schema_id, boot_id, boot_id,
                                         [&schemas, schema_id](const nlohmann::json& value) {
                                           schemas.ValidateById(schema_id, value);
                                         }},
          128);
      camera_detection_writers[device_id] =
          std::make_shared<perception::ProductHistoryWriter>(history->TakeWriter());
      camera_detection_histories.emplace(device_id, std::move(history));
    }
    for (const auto& device : registry.devices()) {
      if (device.identity().device_id != default_object_camera_device_id) continue;
      head_detection_history = std::make_unique<perception::ProductHistory>(
          perception::ProductDeclaration{"robots/" + robot_name + "/detection/head/state", schema_id,
                                         boot_id, boot_id,
                                         [&schemas, schema_id](const nlohmann::json& value) {
                                           schemas.ValidateById(schema_id, value);
                                         }},
          128);
      head_detection_writer = std::make_shared<perception::ProductHistoryWriter>(
          head_detection_history->TakeWriter());
      break;
    }
  }
  if (microphone_vad_available) {
    const std::string schema_id = "egia.contracts.perception.world.vad.v1";
    vad_history = std::make_unique<perception::ProductHistory>(
        perception::ProductDeclaration{"robots/" + robot_name + "/world/vad/state", schema_id,
                                       boot_id, boot_id,
                                       [&schemas, schema_id](const nlohmann::json& value) {
                                         schemas.ValidateById(schema_id, value);
                                       }},
        128);
    vad_writer = std::make_shared<perception::ProductHistoryWriter>(vad_history->TakeWriter());
  }
  if (microphone_vad_available && microphone_asr_available) {
    const std::string schema_id = "egia.contracts.perception.world.asr.v1";
    asr_history = std::make_unique<perception::ProductHistory>(
        perception::ProductDeclaration{"robots/" + robot_name + "/world/asr/state", schema_id,
                                       boot_id, boot_id,
                                       [&schemas, schema_id](const nlohmann::json& value) {
                                         schemas.ValidateById(schema_id, value);
                                       }},
        128);
    asr_writer = std::make_shared<perception::ProductHistoryWriter>(asr_history->TakeWriter());
  }

  std::mutex publication_mutex;
  std::chrono::system_clock::time_point detection_observed_at;
  std::chrono::system_clock::time_point head_detection_observed_at;
  std::chrono::system_clock::time_point vad_observed_at;
  std::chrono::system_clock::time_point asr_observed_at;
  const auto monotonic_now = [](std::chrono::system_clock::time_point& previous) {
    const auto now = std::chrono::system_clock::now();
    if (now > previous) previous = now;
    return previous;
  };
  actionlist::Manager* action_lists_ptr = nullptr;
  const auto publish_detection = [&] {
    if (!detection_writer) return;
    try {
      std::lock_guard<std::mutex> lock(publication_mutex);
      detection_writer->Append(monotonic_now(detection_observed_at),
                               body.BuildDetection(registry.robot().id));
    } catch (const std::exception& error) {
      lib::LogEvent(log, spdlog::level::warn, "detection_publication_failed",
                    {{"reason", error.what()}});
    }
    if (action_lists_ptr != nullptr) action_lists_ptr->NotifyExternalState();
  };
  camera_pose.SetUpdatedCallback(publish_detection);
  camera_object.SetUpdatedCallback([&](const std::string& device_id) {
    try {
      std::lock_guard<std::mutex> lock(publication_mutex);
      const auto writer = camera_detection_writers.find(device_id);
      if (writer == camera_detection_writers.end()) return;
      auto projection = body.BuildCameraObjectDetection(registry.robot().id, device_id);
      writer->second->Append(monotonic_now(head_detection_observed_at), projection);
      const auto* camera = registry.FindDevice(device_id);
      if (head_detection_writer && camera && camera->part().part_id == "head")
        head_detection_writer->Append(monotonic_now(head_detection_observed_at), projection);
    } catch (const std::exception& error) {
      lib::LogEvent(log, spdlog::level::warn, "head_detection_publication_failed",
                    {{"reason", error.what()}});
    }
  });
  microphone_vad.SetUpdatedCallback([&](bool asr_updated) {
    try {
      std::lock_guard<std::mutex> lock(publication_mutex);
      if (vad_writer) vad_writer->Append(monotonic_now(vad_observed_at), body.BuildVad());
      if (asr_updated && asr_writer)
        asr_writer->Append(monotonic_now(asr_observed_at), body.BuildAsr());
    } catch (const std::exception& error) {
      lib::LogEvent(log, spdlog::level::warn, "audio_publication_failed",
                    {{"reason", error.what()}});
    }
  });

  std::vector<perception::ProducerSource> producer_sources;
  auto camera_pose_pump = std::make_shared<std::shared_ptr<models::CameraPosePump>>();
  if (camera_pose_available) {
    producer_sources.push_back(
        {"human_detection", {}, [&, camera_pose_pump] {
           const std::string part_id = config.camera_pose.camera_part_id;
           const std::string device_id =
               registry.Find(hardware::DeviceKind::kCamera, part_id).identity().device_id;
           const auto configured_interval =
               std::chrono::milliseconds(1000 / config.camera_pose.rate_hz);
           const auto capture_interval =
               camera_stream_requested
                   ? configured_interval
                   : std::max(configured_interval, std::chrono::milliseconds(1000));
           auto pump = std::make_shared<models::CameraPosePump>(
               models,
               [&registry, &telemetry, part_id,
                gimbal_part_id = config.camera_pose.gimbal_part_id] {
                 try {
                   const auto& device = registry.Find(hardware::DeviceKind::kCamera, part_id);
                   auto frame = telemetry.LatestCamera(device.identity().device_id);
                   if (frame) {
                     try {
                       const auto& gimbal =
                           registry.Find(hardware::DeviceKind::kGimbal, gimbal_part_id);
                       if (gimbal.robot_id() == device.robot_id()) {
                         frame->capture_gimbal = gimbal.feeds().ReadGimbalTelemetry();
                         if (frame->capture_gimbal) {
                           frame->capture_gimbal_sampled_monotonic_ns =
                               std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
                         }
                       }
                     } catch (const std::exception&) {
                       // A fixed camera or temporarily unavailable gimbal is valid.
                     }
                   }
                   return frame;
                 } catch (const std::exception&) {
                   return std::optional<hardware::CameraTelemetry>{};
                 }
               },
               camera_pose, config.camera_pose.detection_model_id, device_id, capture_interval,
               camera_face_model_id,
               camera_pose_enrichment_available ? config.camera_pose.model_id : std::string{},
               camera_posture_available ? config.camera_pose.posture_model_id : std::string{});
           *camera_pose_pump = pump;
           return [camera_pose_pump, pump = std::move(pump)]() mutable {
             camera_pose_pump->reset();
             pump.reset();
           };
         }});
    if (camera_pose_enrichment_available) {
      producer_sources.push_back(
          {"human_pose", {"human_detection"}, [camera_pose_pump, &config] {
             const auto pump = *camera_pose_pump;
             if (!pump) throw std::logic_error("camera pose producer dependency is not running");
             pump->SetPoseEnabled(true, config.perception_stream.human_pose
                                            ? std::chrono::milliseconds(0)
                                            : std::chrono::milliseconds(1000));
             return [camera_pose_pump] {
               if (const auto pump = *camera_pose_pump) pump->SetPoseEnabled(false);
             };
           }});
    }
    if (!camera_face_model_id.empty()) {
      producer_sources.push_back(
          {"face_recognition", {"human_detection"}, [camera_pose_pump] {
             const auto pump = *camera_pose_pump;
             if (!pump) throw std::logic_error("camera pose producer dependency is not running");
             pump->SetFaceEnabled(true, std::chrono::milliseconds(250));
             return [camera_pose_pump] {
               if (const auto pump = *camera_pose_pump) pump->SetFaceEnabled(false);
             };
           }});
    }
    std::vector<std::string> watch_dependencies{"human_detection"};
    if (camera_pose_enrichment_available) watch_dependencies.push_back("human_pose");
    if (!camera_face_model_id.empty()) watch_dependencies.push_back("face_recognition");
    producer_sources.push_back(
        {"human_detection_watch", std::move(watch_dependencies), [] { return [] {}; }});
  }
  if (camera_object_available) {
    for (const auto& camera : registry.devices()) {
      if (camera.identity().kind != hardware::DeviceKind::kCamera) continue;
      const std::string device_id = camera.identity().device_id;
      const std::string part_id = camera.part().part_id;
      producer_sources.push_back(
        {"camera_object:" + device_id, {}, [&, device_id, part_id] {
           auto pump = std::make_shared<models::CameraObjectPump>(
               models,
               [&registry, &telemetry, device_id] {
                 try {
                   const auto* device = registry.FindDevice(device_id);
                   if (!device || !device->ReadHealth().alive)
                     return std::optional<hardware::CameraTelemetry>{};
                   const auto frame = telemetry.SnapshotCamera(device_id);
                   if (!frame || !models::FreshCameraFrame(
                                     frame->received_at, std::chrono::steady_clock::now()))
                     return std::optional<hardware::CameraTelemetry>{};
                   return std::optional<hardware::CameraTelemetry>{frame->telemetry};
                 } catch (const std::exception&) {
                   return std::optional<hardware::CameraTelemetry>{};
                 }
               },
               camera_object, config.camera_object.model_id, device_id, part_id,
               std::chrono::milliseconds(1000 / config.camera_object.rate_hz));
           return [pump = std::move(pump)]() mutable { pump.reset(); };
         }});
    }
  }
  if (microphone_vad_available) {
    producer_sources.push_back(
        {"microphone_vad", {}, [&] {
           auto pump = std::make_shared<models::MicrophoneVadPump>(
               models,
               [&registry, &telemetry] {
                 try {
                   const auto& device = registry.Find(hardware::DeviceKind::kMicrophone);
                   return telemetry.ConsumeMicrophone(device.identity().device_id, "vad", true);
                 } catch (const std::exception&) {
                   return std::optional<hardware::MicrophoneTelemetry>{};
                 }
               },
               microphone_vad, config.microphone_vad.model_id,
               std::chrono::milliseconds(config.microphone_vad.window_ms),
               std::chrono::milliseconds(10),
               microphone_asr_available ? config.microphone_vad.asr_model_id : std::string{},
               config.microphone_vad.save_audio ? run_log_dir / "attachments"
                                                : std::filesystem::path{},
               config.microphone_vad.overwrite_audio);
           return [pump = std::move(pump)]() mutable { pump.reset(); };
         }});
  }
  perception::ProducerCoordinator producers(std::move(producer_sources));
  if (detection_history) {
    const auto* detection_model = models.Find(config.camera_pose.detection_model_id);
    resources.Register(
        detection_history->Reader(), [&] { return producers.Acquire("human_detection_watch"); },
        detection_model->timeout + std::chrono::milliseconds(1000 / config.camera_pose.rate_hz));
  }
  if (!camera_detection_histories.empty()) {
    const auto* object_model = models.Find(config.camera_object.model_id);
    for (const auto& [device_id, history] : camera_detection_histories) {
      resources.Register(
          history->Reader(), [&, device_id] { return producers.Acquire("camera_object:" + device_id); },
          object_model->timeout + std::chrono::milliseconds(1000 / config.camera_object.rate_hz));
    }
  }
  if (head_detection_history) {
    const auto* object_model = models.Find(config.camera_object.model_id);
    const auto* object_camera = registry.FindDevice(default_object_camera_device_id);
    resources.Register(
        head_detection_history->Reader(), [&, object_camera] {
          if (!object_camera) throw std::runtime_error("default object camera is not configured");
          return producers.Acquire("camera_object:" + object_camera->identity().device_id);
        },
        object_model->timeout + std::chrono::milliseconds(1000 / config.camera_object.rate_hz));
  }
  if (vad_history) {
    const auto* vad_model = models.Find(config.microphone_vad.model_id);
    resources.Register(
        vad_history->Reader(), [&] { return producers.Acquire("microphone_vad"); },
        vad_model->timeout + std::chrono::milliseconds(config.microphone_vad.window_ms) +
            std::chrono::milliseconds(10));
  }
  if (asr_history) {
    const auto* vad_model = models.Find(config.microphone_vad.model_id);
    const auto* asr_model = models.Find(config.microphone_vad.asr_model_id);
    resources.Register(
        asr_history->Reader(), [&] { return producers.Acquire("microphone_vad"); },
        2 * vad_model->timeout + asr_model->timeout +
            2 * std::chrono::milliseconds(config.microphone_vad.window_ms) +
            std::chrono::milliseconds(20));
  }
  std::vector<perception::ProducerLease> always_on;
  if (config.perception_stream.human && camera_pose_available)
    always_on.push_back(producers.Acquire("human_detection"));
  if (config.perception_stream.human_pose && camera_pose_enrichment_available)
    always_on.push_back(producers.Acquire("human_pose"));
  if (config.perception_stream.face && !camera_face_model_id.empty())
    always_on.push_back(producers.Acquire("face_recognition"));
  if (config.perception_stream.vad && microphone_vad_available)
    always_on.push_back(producers.Acquire("microphone_vad"));
  LogModuleStatus(log, "body", "ready", {{"devices", registry.devices().size()}});
  dispatch::Dispatcher dispatcher(registry, schemas, config.mode.expose_operator, &models,
                                  paths.state / "assets", paths.state / "primitive-commands.jsonl");
  dispatcher.SetTelemetryHub(&telemetry);
  LogModuleStatus(log, "dispatcher", "ready", {{"expose_operator", config.mode.expose_operator}});
  const skills::native::ResourceReader skill_resource_reader =
      [&resources](const std::string& uri) { return resources.ReadResource(uri, false); };
  const std::string skill_chat_model_id = ResolveSkillChatModel(models);
  const std::string skill_vlm_model_id = ResolveSkillVlmModel(models);
  const skills::native::InferenceRunner inference_runner =
      [&models, &camera_pose, vad_model_id = config.microphone_vad.model_id,
       human_model_id = config.camera_pose.detection_model_id,
       human_pose_model_id = config.camera_pose.model_id,
       face_model_id = config.camera_face.model_id,
       chat_model_id = skill_chat_model_id, vlm_model_id = skill_vlm_model_id](const skills::InferenceRequest& inference,
                                             skills::Cancelled cancelled) {
        return RunSkillInference(models, camera_pose,
                                 {vad_model_id, human_model_id, human_pose_model_id, face_model_id,
                                  chat_model_id, vlm_model_id},
                                 inference, std::move(cancelled));
      };
  skills::builtin::RuntimeCapabilities builtin_capabilities;
  builtin_capabilities.grasp_observe_joints = [angles = config.arm_grasp.observe_joint_positions_rad] {
    if (angles.size() != 6) throw std::runtime_error("grasp observation joint pose is not configured");
    return nlohmann::json(angles);
  };
  builtin_capabilities.grasp_wrist_camera = [camera = config.arm_grasp.wrist_camera_device_id] {
    if (camera.empty()) throw std::runtime_error("grasp wrist camera is not configured");
    return camera;
  };
  builtin_capabilities.grasp_workspace_bounds = [minimum = config.arm_grasp.workspace_min_m,
                                                  maximum = config.arm_grasp.workspace_max_m] {
    if (minimum.size() != 3 || maximum.size() != 3)
      throw std::runtime_error("grasp workspace is not configured");
    return nlohmann::json{{"min_m", minimum}, {"max_m", maximum}};
  };
  builtin_capabilities.plan_grasp = [&models](nlohmann::json request,
                                               skills::Cancelled cancelled) {
    constexpr char kPlannerId[] = "arm-grasp-planner";
    if (models.Resolve(kPlannerId, models::Capability::kArmGraspPlan) !=
        models::ModelReferenceState::kEnabled)
      throw std::runtime_error("arm grasp planner is not enabled");
    const auto result = models.Call(kPlannerId, request, std::move(cancelled));
    if (result.status != models::CallResult::Status::kCompleted)
      throw std::runtime_error(result.message.empty() ? "arm grasp planning failed" : result.message);
    return result.response;
  };
  builtin_capabilities.default_object_camera = [&registry, part_id = config.camera_object.camera_part_id] {
    return registry.Find(hardware::DeviceKind::kCamera, part_id).identity().device_id;
  };
  const auto observe_object_frame =
      [&registry, &models, &body, model_id = config.camera_object.model_id](
          const std::string& device_id, const auto& frame, skills::Cancelled cancelled,
          bool planner_received = false) {
        if (model_id.empty()) throw std::runtime_error("object detection model is not configured");
        const auto* device = registry.FindDevice(device_id);
        if (!device || device->identity().kind != hardware::DeviceKind::kCamera)
          throw std::runtime_error("camera device is not configured: " + device_id);
        if (!device->ReadHealth().alive)
          throw std::runtime_error("camera device is unhealthy: " + device_id);
        const auto maximum_age = planner_received ? std::chrono::seconds(6)
                                                  : std::chrono::seconds(2);
        if (!models::FreshCameraFrame(frame.received_at, std::chrono::steady_clock::now(),
                                      maximum_age))
          throw std::runtime_error("camera frame is unavailable or stale: " + device_id);
        const auto& image = frame.telemetry;
        if (image.width_px == 0 || image.height_px == 0 || image.frame_id.empty() ||
            !image.source_timestamp_ns || *image.source_timestamp_ns <= 0 ||
            image.jpeg.size() < 4 || image.jpeg[0] != 0xff || image.jpeg[1] != 0xd8 ||
            image.jpeg[image.jpeg.size() - 2] != 0xff || image.jpeg.back() != 0xd9)
          throw std::runtime_error("camera frame has invalid metadata or JPEG payload");
        auto result = models.Call(model_id,
                                  {{"input", {{"data_b64", Base64Encode(image.jpeg)},
                                              {"filename", "camera.jpg"},
                                              {"media_type", "image/jpeg"}}}},
                                  std::move(cancelled));
        if (result.status != models::CallResult::Status::kCompleted)
          throw std::runtime_error(result.message.empty() ? "object inference failed"
                                                         : result.message);
        if (!result.response.is_object() || !result.response.contains("objects") ||
            !result.response.at("objects").is_array())
          throw std::runtime_error("object model returned invalid objects");
        models::CameraObjectStore::Snapshot snapshot;
        snapshot.model_id = model_id;
        snapshot.device_id = device_id;
        snapshot.part_id = device->part().part_id;
        snapshot.sequence = 1;
        snapshot.generation = 1;
        snapshot.frame_id = image.frame_id;
        snapshot.source_timestamp_ns = image.source_timestamp_ns;
        snapshot.source_clock = image.source_clock;
        snapshot.source_clock_id = image.source_clock_id;
        snapshot.image_width_px = image.width_px;
        snapshot.image_height_px = image.height_px;
        snapshot.response = std::move(result.response);
        return body.ProjectCameraObjectDetection(registry.robot().id, snapshot);
      };
  builtin_capabilities.observe_objects =
      [&telemetry, observe_object_frame](const std::string& device_id,
                                         skills::Cancelled cancelled) {
        const auto frame = telemetry.SnapshotCamera(device_id);
        if (!frame) throw std::runtime_error("camera frame is unavailable or stale: " + device_id);
        return observe_object_frame(device_id, *frame, std::move(cancelled));
      };
  builtin_capabilities.observe_objects_fresh =
      [&telemetry, observe_object_frame](
          const std::string& device_id, std::chrono::steady_clock::time_point after,
          skills::Cancelled cancelled) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
        while (std::chrono::steady_clock::now() < deadline) {
          if (cancelled && cancelled()) throw std::runtime_error("fresh observation cancelled");
          const auto frame = telemetry.SnapshotCamera(device_id);
          if (frame && frame->received_at > after &&
              models::FreshCameraFrame(frame->received_at, std::chrono::steady_clock::now(),
                                       std::chrono::seconds(2)))
            return observe_object_frame(device_id, *frame, std::move(cancelled));
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        throw std::runtime_error("no fresh camera frame arrived after observation motion");
      };
  builtin_capabilities.execute_primitive =
      [&dispatcher](const skills::PrimitiveReference& primitive, const std::string& device,
                    nlohmann::json arguments, const std::string& command_id,
                    skills::Cancelled cancelled) {
        auto result = dispatcher.ExecuteCommand(primitive.name, primitive.version, device,
                                                std::move(arguments), command_id, cancelled);
        if (result.status != dispatch::DispatchResult::Status::kCompleted ||
            !result.payload.is_object())
          throw std::runtime_error(result.message.empty() ? "primitive dispatch failed"
                                                          : result.message);
        return result.payload;
      };
  builtin_capabilities.cancel_primitive = [&dispatcher](const std::string& command_id) {
    (void)dispatcher.CancelCommand(command_id);
  };
  builtin_capabilities.cancel_navigation_command =
      [&dispatcher](const std::string& command_id, const std::string& device_id) {
        switch (dispatcher.CancelActiveNavigationCommand(command_id, device_id)) {
          case dispatch::NavigationCancelStatus::kCancelled:
            return skills::builtin::NavigationCancelStatus::kCancelled;
          case dispatch::NavigationCancelStatus::kNotFound:
            return skills::builtin::NavigationCancelStatus::kNotFound;
          case dispatch::NavigationCancelStatus::kMismatch:
            return skills::builtin::NavigationCancelStatus::kMismatch;
          case dispatch::NavigationCancelStatus::kUncertain:
            return skills::builtin::NavigationCancelStatus::kUncertain;
        }
        return skills::builtin::NavigationCancelStatus::kUncertain;
      };
  builtin_capabilities.read_resource = skill_resource_reader;
  builtin_capabilities.open_resource_subscription = [&resources](const std::string& uri) {
    auto reader = resources.FindHistory(uri);
    if (!reader) throw std::runtime_error("resource has no retained history: " + uri);
    std::shared_ptr<void> lifetime;
    if (auto demand = resources.AcquireDemand(uri)) {
      lifetime = std::make_shared<perception::ProducerLease>(std::move(*demand));
    } else {
      lifetime = std::make_shared<int>(0);
    }
    const auto publication = [](const perception::ProductPtr& product) {
      return skills::builtin::ResourcePublication{product->sequence, product->observed_at,
                                                  product->boot_id, product->epoch, product->value};
    };
    const auto retention = [](const perception::RetentionState& state) {
      return skills::builtin::ResourceRetention{state.first_available_sequence,
                                                state.latest_sequence,
                                                state.evicted_through_sequence, state.closed};
    };
    skills::builtin::RuntimeCapabilities::ResourceAccess access;
    access.lifetime = std::move(lifetime);
    access.read_history = [reader = *reader, publication,
                           retention](const skills::builtin::ResourceSelector& selector) {
      skills::builtin::ResourceHistory result;
      const auto one = [&](perception::ProductLookup lookup) {
        result.retention = retention(lookup.retention);
        result.retention_gap = lookup.retention_gap;
        if (lookup.product) result.publications.push_back(publication(lookup.product));
      };
      const auto range = [&](perception::ProductRange found) {
        result.retention = retention(found.retention);
        result.retention_gap = found.retention_gap;
        result.truncated = found.truncated;
        for (const auto& product : found.products)
          result.publications.push_back(publication(product));
      };
      switch (selector.kind) {
        case skills::builtin::ResourceSelectorKind::kLatest:
          one(reader.Latest());
          break;
        case skills::builtin::ResourceSelectorKind::kExactSequence:
          one(reader.AtSequence(selector.first_sequence));
          break;
        case skills::builtin::ResourceSelectorKind::kExactTime:
          one(reader.AtTime(selector.first_time));
          break;
        case skills::builtin::ResourceSelectorKind::kBefore:
          one(reader.Before(selector.first_time));
          break;
        case skills::builtin::ResourceSelectorKind::kAfter:
          one(reader.After(selector.first_time));
          break;
        case skills::builtin::ResourceSelectorKind::kNearest:
          one(reader.Nearest(selector.first_time));
          break;
        case skills::builtin::ResourceSelectorKind::kSequenceRange:
          range(reader.RangeBySequence(selector.first_sequence, selector.last_sequence,
                                       selector.limit));
          break;
        case skills::builtin::ResourceSelectorKind::kTimeRange:
          range(reader.RangeByTime(selector.first_time, selector.last_time, selector.limit));
          break;
      }
      return result;
    };
    access.wait_publication = [reader = *reader, publication, retention](
                                  std::uint64_t after_sequence, std::chrono::milliseconds timeout,
                                  skills::Cancelled cancelled) {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      const auto stopped = [&] {
        return (cancelled && cancelled()) || std::chrono::steady_clock::now() >= deadline;
      };
      const auto waited = reader.WaitNewer(after_sequence, stopped);
      skills::builtin::ResourceWait result;
      result.retention = retention(waited.retention);
      result.retention_gap = waited.retention_gap;
      if (waited.status == perception::WaitStatus::kProduct) {
        result.status = skills::builtin::ResourceWaitStatus::kPublication;
        result.publication = publication(waited.product);
      } else if (cancelled && cancelled()) {
        result.status = skills::builtin::ResourceWaitStatus::kCancelled;
      } else if (waited.status == perception::WaitStatus::kClosed || waited.retention.closed) {
        result.status = skills::builtin::ResourceWaitStatus::kClosed;
      } else {
        result.status = skills::builtin::ResourceWaitStatus::kTimeout;
      }
      return result;
    };
    return access;
  };
  builtin_capabilities.infer_once = inference_runner;
  builtin_capabilities.set_detection_overlay = [&](const std::string& execution_id,
                                                    nlohmann::json correction) {
    const auto latest = camera_pose.Latest();
    if (latest) correction["activation_generation"] = latest->generation;
    detection_overlays.Upsert(execution_id, std::move(correction));
    publish_detection();
  };
  builtin_capabilities.clear_detection_overlay = [&](const std::string& execution_id) {
    detection_overlays.Clear(execution_id);
    publish_detection();
  };
  builtin_capabilities.log_detection_attachment = [&camera_pose](nlohmann::json observation) {
    if (!observation.is_object() || !observation.contains("source") ||
        !observation.at("source").is_string() ||
        observation.at("source").get_ref<const std::string&>().empty() ||
        !observation.contains("source_sequence") ||
        !observation.at("source_sequence").is_number_unsigned() ||
        !observation.contains("source_generation") ||
        !observation.at("source_generation").is_number_unsigned() ||
        !observation.contains("bbox") || !observation.at("bbox").is_array() ||
        observation.at("bbox").size() != 4)
      throw std::invalid_argument("invalid stable human observation");
    const auto sequence = observation.at("source_sequence").get<std::uint64_t>();
    const auto generation = observation.at("source_generation").get<std::uint64_t>();
    auto snapshot = camera_pose.Find(observation.at("source").get_ref<const std::string&>(),
                                     sequence, generation);
    if (!snapshot || !snapshot->jpeg || snapshot->jpeg->empty()) {
      observation["attachment_error"] = "stable human detection frame is unavailable";
      lib::LogEvent(lib::GetLogger("skills.observe_human"), spdlog::level::warn,
                    "stable_human_observed", std::move(observation));
      return;
    }
    lib::LogEventWithAttachment(lib::GetLogger("skills.observe_human"), spdlog::level::info,
                                "stable_human_observed", std::move(observation), "vision", ".jpg",
                                "image/jpeg", *snapshot->jpeg);
  };
  lib::TimerOptions skill_timer_options;
  skill_timer_options.callback_workers = 4;
  lib::ThreadTimerService skill_timers(clock, std::move(skill_timer_options));
  skills::builtin::Registry builtin_skills(skill_timers, std::move(builtin_capabilities));
  island::skills::native::RegisterSkills(builtin_skills);
  skills::native::Runtime skill_runtime(dispatcher, skill_resource_reader, inference_runner);
  if (options.skills_dir.empty()) {
    if (const char* configured = std::getenv("EOS_SKILLS")) options.skills_dir = configured;
  }
  if (!options.skills_dir.empty()) {
    const std::filesystem::path skills_path(options.skills_dir);
    if (!skills_path.is_absolute()) {
      throw std::runtime_error("skill directory must be absolute: " + skills_path.string());
    }
    try {
      skill_runtime = skills::native::Runtime::LoadDirectory(
          skills_path, dispatcher, skill_resource_reader,
          [&resources](const std::string& uri_template) {
            return resources.SupportsResourceTemplate(uri_template);
          },
          inference_runner);
    } catch (const std::exception& error) {
      LogModuleStatus(log, "skills", "failed",
                      {{"path", skills_path.string()}, {"reason", error.what()}});
      throw;
    }
    const std::vector<skills::native::Metadata> discovered_skills = skill_runtime.Discover();
    nlohmann::json skills = nlohmann::json::array();
    for (const skills::native::Metadata& skill : discovered_skills) {
      skills.push_back({{"name", skill.name},
                        {"version", skill.version},
                        {"library", skill.library},
                        {"library_version", skill.library_version},
                        {"primitives", skill.primitives.size()},
                        {"resources", skill.resources.size()}});
    }
    LogModuleStatus(log, "skills", "ready",
                    {{"path", skills_path.string()}, {"skills", std::move(skills)}});
  } else {
    LogModuleStatus(log, "skills", "disabled", {{"reason", "directory_not_configured"}});
  }
  const std::vector<models::ModelConfig> enabled_models = models.Discover();
  nlohmann::json enabled_model_ids = nlohmann::json::array();
  for (const auto& model : enabled_models) enabled_model_ids.push_back(model.id);
  LogModuleStatus(log, "models", enabled_models.empty() ? "disabled" : "ready",
                  {{"configured", config.models.size()},
                   {"enabled", enabled_models.size()},
                   {"model_ids", std::move(enabled_model_ids)}});

  northbound::NorthboundServices services;
  services.projection = &body;
  services.dispatcher = &dispatcher;
  services.registry = &registry;
  services.models = &models;
  services.telemetry = &telemetry;
  services.resources = &resources;
  services.navigation_points = &navigation_points;
  services.camera_pose = &camera_pose;
  services.camera_object = &camera_object;
  services.perception_roles = {
      {"vad", config.microphone_vad.model_id, config.perception_stream.vad},
      {"human", config.camera_pose.detection_model_id, config.perception_stream.human},
      {"pose", config.camera_pose.model_id, config.perception_stream.human_pose},
      {"face", config.camera_face.model_id, config.perception_stream.face}};
  if (camera_pose_available) {
    services.perception_camera_device_id =
        registry.Find(hardware::DeviceKind::kCamera, config.camera_pose.camera_part_id)
            .identity().device_id;
  }
  if (camera_object_available) {
    services.object_model_id = config.camera_object.model_id;
  }
  actionlist::PrimitiveRunner primitive_runner;
  primitive_runner.execute = [&dispatcher](const std::string& name, const std::string& version,
                                           const std::string& device_id, nlohmann::json arguments,
                                           const std::string& command_id,
                                           const std::function<bool()>& cancelled) {
    auto result = dispatcher.ExecuteCommand(name, version, device_id, std::move(arguments),
                                            command_id, cancelled);
    if (result.status == dispatch::DispatchResult::Status::kInvalidArgument ||
        result.status == dispatch::DispatchResult::Status::kNotFound ||
        result.status == dispatch::DispatchResult::Status::kAlreadyExists ||
        result.status == dispatch::DispatchResult::Status::kFailedPrecondition)
      return nlohmann::json{
          {"type", "primitive.ack"},           {"primitive", name}, {"primitive_version", version},
          {"command_id", command_id},          {"accepted", false}, {"reason", result.message},
          {"result", {{"status", "rejected"}}}};
    if (result.status != dispatch::DispatchResult::Status::kCompleted ||
        !result.payload.is_object())
      throw std::runtime_error(result.message.empty() ? "primitive dispatch failed"
                                                      : result.message);
    return result.payload;
  };
  primitive_runner.cancel = [&dispatcher](const std::string& command_id) {
    dispatcher.CancelCommand(command_id);
  };
  skills::SkillManager* skill_service_ptr = nullptr;
  actionlist::SkillRunner skill_runner;
  skill_runner.start = [&skill_service_ptr](const std::string& name, const std::string& version,
                                            nlohmann::json arguments,
                                            const std::string& request_id) {
    return skill_service_ptr->Start(name, version, std::move(arguments), request_id);
  };
  skill_runner.cancel = [&skill_service_ptr](const std::string& execution_id) {
    return skill_service_ptr->Terminate(execution_id);
  };
  skill_runner.list_executions = [&skill_service_ptr] {
    return skill_service_ptr->ListExecutions();
  };
  actionlist::ManagerOptions action_list_options;
  if (config.workflow_journal_enabled)
    action_list_options.journal_directory = paths.state / "workflows";
  action_list_options.read_resource =
      [&detection_history,
       robot_name](const std::string& uri) -> std::optional<actionlist::ResourceSnapshot> {
    if (uri != "robots/" + robot_name + "/detection/state" || !detection_history) return {};
    const auto latest = detection_history->Reader().Latest();
    if (!latest.product) return {};
    return actionlist::ResourceSnapshot{latest.product->value, latest.product->observed_at};
  };
  actionlist::Manager action_lists(std::move(skill_runner), std::move(primitive_runner),
                                   std::move(action_list_options));
  action_lists_ptr = &action_lists;
  skills::SkillManagerObservers observers;
  observers.execution = [&action_lists](const skills::ExecutionSnapshot& execution) {
    action_lists.ObserveExecution(execution);
  };
  observers.emitted_signal = [&action_lists](const skills::ExecutionSnapshot& execution,
                                             const skills::Signal& signal) {
    action_lists.ObserveSignal(execution, signal);
  };
  skills::SkillManagerOptions skill_options;
  skill_options.journal_log_directory = process_log_dir;
  skill_options.journal_log_overwrite = log_config.overwrite;
  skill_options.journal_log_rotation_size_bytes = log_config.rotation_size_bytes;
  skill_options.journal_log_rotation_files = log_config.rotation_files;
  std::vector<skills::SkillRuntime> skill_runtimes;
  skill_runtimes.push_back({"builtin", builtin_skills});
  if (!skill_runtime.Discover().empty()) skill_runtimes.push_back({"native", skill_runtime});
  skills::SkillManager skill_service(std::move(skill_runtimes), std::move(skill_options),
                                     std::move(observers));
  skill_service_ptr = &skill_service;
  const auto tick = [&registry, &dispatcher] {
    constexpr double kDtS = 0.1;
    dispatcher.TickHardware(kDtS);
    dispatcher.Tick(kDtS);
  };
  internal::NorthboundServer northbound(
      options.host, options.grpc_port, services, tick, &skill_service, &action_lists,
      paths.contracts / "workflows", {config.grpc_uds_path, config.grpc_uds_mode});
  std::atomic<bool> failed{false};
  std::thread northbound_thread([&] {
    try {
      northbound.Run();
    } catch (const std::exception& error) {
      lib::LogEvent(log, spdlog::level::err, "northbound_stage_failed", {{"reason", error.what()}});
      failed.store(true);
    }
  });
  while (!northbound.ready() && !failed.load() && !stop_requested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!northbound.ready()) {
    lib::LogEvent(log, spdlog::level::err, "runtime_start_failed", {{"stage", "northbound"}});
    northbound.Stop();
    northbound_thread.join();
    return 1;
  }
  lib::LogEvent(log, spdlog::level::info, "runtime_started",
                {{"robot", registry.robot().id},
                 {"devices", registry.devices().size()},
                 {"expose_operator", config.mode.expose_operator},
                 {"host", options.host},
                 {"grpc_port", northbound.grpc_port()},
                 {"grpc_uds_path", config.grpc_uds_path}});
  LogModuleStatus(log, "northbound", "ready",
                  {{"host", options.host},
                   {"grpc_port", northbound.grpc_port()},
                   {"grpc_uds_path", config.grpc_uds_path}});

  while (!failed.load() && !stop_requested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  const bool northbound_failed = failed.load();
  lib::LogEvent(log, northbound_failed ? spdlog::level::err : spdlog::level::info,
                "runtime_stopping",
                {{"reason", northbound_failed ? "northbound_failure" : "stop_requested"}});
  northbound.Stop();
  northbound_thread.join();
  lib::LogEvent(log, spdlog::level::info, "runtime_stopped", {{"failed", northbound_failed}});
  return northbound_failed ? 1 : 0;
}

}  // namespace eos::runtime::init
