#include "src/eos/runtime/northbound/service.h"

#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/time_util.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "src/eos/lib/log.h"
#include "src/eos/runtime/dispatch/dispatcher.h"
#include "src/eos/runtime/hardware/registry.h"
#include "src/eos/runtime/hardware/telemetry_hub.h"
#include "src/eos/runtime/init/schemas.h"
#include "src/eos/runtime/models/runtime.h"
#include "src/eos/runtime/models/camera_object.h"
#include "src/eos/runtime/northbound/request_context.h"
#include "src/eos/runtime/northbound/log_status.h"
#include "src/eos/runtime/northbound/subscribe.h"
#include "src/eos/runtime/perception/body/reader.h"
#include "src/eos/runtime/perception/resource_catalog.h"
#include "src/eos/runtime/models/camera_pose.h"
#include "src/eos/runtime/store/navigation_point_store.h"

namespace eos::runtime::northbound {
namespace {

constexpr char kVersion[] = EOS_PROJECT_VERSION;
constexpr std::size_t kMaxHistoryRange = 1000;
constexpr auto kBodySnapshotMaxAge = std::chrono::seconds(1);

bool IsBodyResource(const std::string& uri) {
  if (uri.rfind("robots/", 0) != 0) return false;
  const std::size_t robot_end = uri.find('/', 7);
  if (robot_end == std::string::npos || robot_end == 7) return false;
  const std::string path = uri.substr(robot_end + 1);
  if (path.rfind("devices/", 0) == 0 && path.size() > 14 &&
      path.compare(path.size() - 6, 6, "/state") == 0)
    return true;
  return path == "arm/state" || path == "base/status" || path == "arm/status" ||
         path == "gripper/status";
}

bool IsSemanticResource(const std::string& uri) {
  if (uri.rfind("robots/", 0) != 0) return false;
  const std::size_t robot_end = uri.find('/', 7);
  if (robot_end == std::string::npos || robot_end == 7) return false;
  const std::string path = uri.substr(robot_end + 1);
  if (path == "detection/state" || path == "detection/head/state" ||
      path == "world/vad/state" || path == "world/asr/state")
    return true;
  return path.rfind("cameras/", 0) == 0 && path.size() > 25 &&
         path.compare(path.size() - 17, 17, "/detections/state") == 0;
}

bool HasKnownRobot(const hardware_runtime::Registry& registry, const std::string& uri) {
  const std::size_t robot_end = uri.find('/', 7);
  const std::string robot_name = uri.substr(7, robot_end - 7);
  return std::any_of(registry.robots().begin(), registry.robots().end(),
                     [&](const auto& entry) { return entry.name == robot_name; });
}

bool IsKnownBodyResource(const hardware_runtime::Registry& registry, const std::string& uri) {
  const std::size_t robot_end = uri.find('/', 7);
  const std::string robot_name = uri.substr(7, robot_end - 7);
  const auto robot = std::find_if(registry.robots().begin(), registry.robots().end(),
                                  [&](const auto& entry) { return entry.name == robot_name; });
  if (robot == registry.robots().end()) return false;
  const std::string path = uri.substr(robot_end + 1);
  if (path.rfind("devices/", 0) == 0) {
    const std::string device_id = path.substr(8, path.size() - 14);
    const auto* device = registry.FindDevice(device_id);
    return device != nullptr && device->robot_id() == robot->id &&
           perception::body::SegmentOfKind(device->part().kind) != nullptr;
  }
  hardware::DeviceKind kind = hardware::DeviceKind::kBase;
  if (path == "arm/state" || path == "arm/status")
    kind = hardware::DeviceKind::kManipulator;
  else if (path == "gripper/status")
    kind = hardware::DeviceKind::kGripper;
  return std::any_of(registry.devices().begin(), registry.devices().end(),
                     [&](const auto& device) {
                       return device.robot_id() == robot->id && device.identity().kind == kind;
                     });
}

std::shared_ptr<spdlog::logger>& NorthboundLogger() {
  static auto logger = eos::lib::GetLogger("northbound");
  return logger;
}

enum class HealthState { kStarting, kReady, kDegraded, kStopping };

struct SubsystemStatus {
  std::string name;
  HealthState state;
  double uptime_seconds;
};

const char* StateLiteral(HealthState state) {
  switch (state) {
    case HealthState::kStarting:
      return "starting";
    case HealthState::kReady:
      return "ready";
    case HealthState::kDegraded:
      return "degraded";
    case HealthState::kStopping:
      return "stopping";
  }
  return "degraded";
}

double Round1(double value) { return std::round(value * 10.0) / 10.0; }

nlohmann::json NavigationPointJson(const store::NavigationPoint& point) {
  return {{"name", point.name}, {"x_m", point.x_m}, {"y_m", point.y_m}, {"yaw_rad", point.yaw_rad}};
}

RpcResult StoreFailure(const store::StoreError& error) {
  switch (error.code()) {
    case store::StoreError::Code::kInvalid:
      return RpcResult::Error(grpc::StatusCode::INVALID_ARGUMENT, error.what());
    case store::StoreError::Code::kNotFound:
      return RpcResult::Error(grpc::StatusCode::NOT_FOUND, error.what());
    case store::StoreError::Code::kConflict:
      return RpcResult::Error(grpc::StatusCode::FAILED_PRECONDITION, error.what());
    case store::StoreError::Code::kExhausted:
      return RpcResult::Error(grpc::StatusCode::RESOURCE_EXHAUSTED, error.what());
    case store::StoreError::Code::kIo:
      return RpcResult::Error(grpc::StatusCode::INTERNAL, error.what());
  }
  return RpcResult::Error(grpc::StatusCode::INTERNAL, error.what());
}

nlohmann::json RangeJson(const hardware_runtime::NumericRange& range) {
  nlohmann::json value = nlohmann::json::object();
  if (range.minimum) value["minimum"] = *range.minimum;
  if (range.maximum) value["maximum"] = *range.maximum;
  if (range.minimum_exclusive) value["minimumExclusive"] = true;
  if (range.maximum_exclusive) value["maximumExclusive"] = true;
  return value;
}

const char* DeviceHealthLiteral(const hardware_runtime::DeviceHealth& health) {
  if (!health.alive) return "unavailable";
  return health.admitted ? "ready" : "degraded";
}

const char* ModelHealthLiteral(models::HealthStatus health) {
  switch (health) {
    case models::HealthStatus::kReady:
      return "ready";
    case models::HealthStatus::kChecking:
    case models::HealthStatus::kDegraded:
      return "degraded";
    case models::HealthStatus::kUnavailable:
      return "unavailable";
  }
  return "unavailable";
}

std::string ModelHealth(const models::Runtime* runtime, const std::string& model_id) {
  if (runtime == nullptr || model_id.empty()) return "unavailable";
  const std::optional<models::ModelHealth> health = runtime->Health(model_id);
  return health ? ModelHealthLiteral(health->status) : "unavailable";
}

grpc::Status AsStatus(const RpcResult& result) {
  return result.ok() ? grpc::Status::OK : grpc::Status(result.code, result.message);
}

template <typename Handler>
grpc::Status HandleGrpcRequest(grpc::ServerContext* context, const char* method,
                               Handler&& handler) {
  std::string request_id;
  grpc::Status status = ResolveRequestId(context, &request_id);
  eos::lib::CorrelationScope scope(request_id);
  const auto started = std::chrono::steady_clock::now();
  if (status.ok()) status = handler(scope.id());
  const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
  const nlohmann::json fields{{"method", method},
                              {"status", static_cast<int>(status.error_code())},
                              {"status_name", GrpcStatusName(status.error_code())},
                              {"reason_category", GrpcReasonCategory(status.error_code())},
                              {"duration_ms", duration_ms}};
  if (status.ok()) {
    eos::lib::LogEvent(NorthboundLogger(), spdlog::level::debug, "grpc_request_completed", fields);
  } else {
    eos::lib::LogEvent(NorthboundLogger(), spdlog::level::warn, "grpc_request_rejected", fields,
                       std::string(method) + " failed: " +
                           GrpcStatusName(status.error_code()));
  }
  return status;
}

template <typename Message>
grpc::Status JsonToMessage(const nlohmann::json& value, Message* output) {
  const auto status = google::protobuf::util::JsonStringToMessage(value.dump(), output);
  if (!status.ok()) {
    return grpc::Status(grpc::StatusCode::DATA_LOSS,
                        "cannot encode service response: " + status.ToString());
  }
  return grpc::Status::OK;
}

nlohmann::json StructToJson(const google::protobuf::Struct& value) {
  std::string text;
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  const auto status = google::protobuf::util::MessageToJsonString(value, &text, options);
  if (!status.ok()) return nlohmann::json::object();
  return nlohmann::json::parse(text);
}

RpcResult DispatchResult(dispatch::DispatchResult result) {
  using Status = dispatch::DispatchResult::Status;
  switch (result.status) {
    case Status::kCompleted:
      return {grpc::StatusCode::OK, {}, std::move(result.payload)};
    case Status::kInvalidArgument:
      return RpcResult::Error(grpc::StatusCode::INVALID_ARGUMENT, std::move(result.message));
    case Status::kNotFound:
      return RpcResult::Error(grpc::StatusCode::NOT_FOUND, std::move(result.message));
    case Status::kAlreadyExists:
      return RpcResult::Error(grpc::StatusCode::ALREADY_EXISTS, std::move(result.message));
    case Status::kFailedPrecondition:
      return RpcResult::Error(grpc::StatusCode::FAILED_PRECONDITION, std::move(result.message));
    case Status::kUnavailable:
      return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, std::move(result.message));
    case Status::kInternal:
      return RpcResult::Error(grpc::StatusCode::INTERNAL, std::move(result.message));
  }
  return RpcResult::Error(grpc::StatusCode::INTERNAL, "unknown dispatch status");
}

std::string StartedAt(std::chrono::system_clock::time_point start) {
  const std::time_t t = std::chrono::system_clock::to_time_t(start);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

google::protobuf::Timestamp ProtoTimestamp(std::chrono::system_clock::time_point time) {
  const auto duration = time.time_since_epoch();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
  auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - seconds).count();
  auto seconds_count = seconds.count();
  if (nanos < 0) {
    --seconds_count;
    nanos += 1000000000;
  }
  google::protobuf::Timestamp result;
  result.set_seconds(seconds_count);
  result.set_nanos(static_cast<std::int32_t>(nanos));
  return result;
}

std::string TimestampString(std::chrono::system_clock::time_point time) {
  return google::protobuf::util::TimeUtil::ToString(ProtoTimestamp(time));
}

std::optional<std::chrono::system_clock::time_point> ChronoTimestamp(
    const google::protobuf::Timestamp& timestamp) {
  constexpr std::int64_t kMinProtoSeconds = -62135596800;
  constexpr std::int64_t kMaxProtoSeconds = 253402300799;
  const auto min_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::time_point::min().time_since_epoch())
                               .count();
  const auto max_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::time_point::max().time_since_epoch())
                               .count();
  if (timestamp.seconds() < kMinProtoSeconds || timestamp.seconds() > kMaxProtoSeconds ||
      timestamp.nanos() < 0 || timestamp.nanos() >= 1000000000 ||
      timestamp.seconds() <= min_seconds || timestamp.seconds() >= max_seconds) {
    return std::nullopt;
  }
  return std::chrono::system_clock::time_point(std::chrono::seconds(timestamp.seconds()) +
                                               std::chrono::nanoseconds(timestamp.nanos()));
}

nlohmann::json RetentionJson(const perception::RetentionState& retention) {
  return {{"first_available_sequence", retention.first_available_sequence},
          {"latest_sequence", retention.latest_sequence},
          {"evicted_through_sequence", retention.evicted_through_sequence},
          {"closed", retention.closed}};
}

nlohmann::json ProductJson(const perception::ProductPtr& product) {
  return {{"sequence", product->sequence},
          {"observed_at", TimestampString(product->observed_at)},
          {"boot_id", product->boot_id},
          {"epoch", product->epoch},
          {"value", product->value}};
}

std::string CapturedAt(std::chrono::system_clock::time_point captured) {
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(captured);
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(captured - seconds).count();
  const std::time_t t = std::chrono::system_clock::to_time_t(captured);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char date[24];
  std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm);
  char result[32];
  std::snprintf(result, sizeof(result), "%s.%03lldZ", date, static_cast<long long>(milliseconds));
  return result;
}

std::string Base64Encode(const std::vector<std::uint8_t>& data) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);
  for (std::size_t i = 0; i < data.size(); i += 3) {
    const std::uint32_t a = data[i];
    const std::uint32_t b = i + 1 < data.size() ? data[i + 1] : 0;
    const std::uint32_t c = i + 2 < data.size() ? data[i + 2] : 0;
    const std::uint32_t bits = (a << 16U) | (b << 8U) | c;
    out.push_back(kAlphabet[(bits >> 18U) & 0x3fU]);
    out.push_back(kAlphabet[(bits >> 12U) & 0x3fU]);
    out.push_back(i + 1 < data.size() ? kAlphabet[(bits >> 6U) & 0x3fU] : '=');
    out.push_back(i + 2 < data.size() ? kAlphabet[bits & 0x3fU] : '=');
  }
  return out;
}

std::string Base64Encode(const std::string& data) {
  return Base64Encode(std::vector<std::uint8_t>(data.begin(), data.end()));
}

nlohmann::json MediaJson(const island::eos::v1::PerceptionMedia& media) {
  nlohmann::json result{{"data_b64", Base64Encode(media.data())}};
  if (media.has_filename()) result["filename"] = media.filename();
  if (media.has_media_type()) result["media_type"] = media.media_type();
  return result;
}

nlohmann::json AudioJson(const island::eos::v1::AudioMedia& media) {
  nlohmann::json result{{"data_b64", Base64Encode(media.data())}};
  if (media.has_filename()) result["filename"] = media.filename();
  if (media.has_media_type()) result["media_type"] = media.media_type();
  if (media.has_sample_rate_hz()) result["sample_rate_hz"] = media.sample_rate_hz();
  if (media.has_channels()) result["channels"] = media.channels();
  return result;
}

nlohmann::json BboxJson(const island::eos::v1::BoundingBox& bbox) {
  return nlohmann::json::array({bbox.x1(), bbox.y1(), bbox.x2(), bbox.y2()});
}

nlohmann::json PersonJson(const island::eos::v1::TrackedHuman& person) {
  nlohmann::json result{{"track_id", person.track_id()},
                        {"bbox", BboxJson(person.bbox())},
                        {"confidence", person.confidence()}};
  if (person.has_distance_m()) result["distance_m"] = person.distance_m();
  return result;
}

nlohmann::json TypedPerceptionResponse(nlohmann::json value) {
  auto convert_bbox = [](nlohmann::json* item) {
    const auto bbox = item->at("bbox");
    (*item)["bbox"] = {
        {"x1", bbox.at(0)}, {"y1", bbox.at(1)}, {"x2", bbox.at(2)}, {"y2", bbox.at(3)}};
  };
  auto convert_persons = [&](nlohmann::json* persons, bool pose) {
    for (auto& person : *persons) {
      convert_bbox(&person);
      if (!pose) continue;
      nlohmann::json keypoints = nlohmann::json::array();
      for (const auto& point : person.at("keypoints"))
        keypoints.push_back({{"x", point.at(0)}, {"y", point.at(1)}});
      person["keypoints"] = std::move(keypoints);
    }
  };
  if (value.contains("persons"))
    convert_persons(&value["persons"],
                    !value["persons"].empty() && value["persons"][0].contains("keypoints"));
  if (value.contains("furniture")) {
    for (auto& furniture : value["furniture"]) convert_bbox(&furniture);
  }
  if (value.contains("data") && value["data"].contains("faces")) {
    for (auto& face : value["data"]["faces"]) convert_bbox(&face);
  }
  return value;
}

template <typename Message>
grpc::Status PerceptionResponse(const RpcResult& result, Message* response) {
  if (!result.ok()) return AsStatus(result);
  try {
    return JsonToMessage(TypedPerceptionResponse(result.payload), response);
  } catch (const nlohmann::json::exception& error) {
    return grpc::Status(grpc::StatusCode::DATA_LOSS,
                        "model returned an invalid typed response: " + std::string(error.what()));
  }
}

RpcResult ModelCallResult(models::CallResult result) {
  using Status = models::CallResult::Status;
  switch (result.status) {
    case Status::kCompleted:
      return {grpc::StatusCode::OK, {}, std::move(result.response)};
    case Status::kInvalidArgument:
      return RpcResult::Error(grpc::StatusCode::INVALID_ARGUMENT, std::move(result.message));
    case Status::kNotFound:
      return RpcResult::Error(grpc::StatusCode::NOT_FOUND, std::move(result.message));
    case Status::kUnavailable:
      return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, std::move(result.message));
    case Status::kDeadlineExceeded:
      return RpcResult::Error(grpc::StatusCode::DEADLINE_EXCEEDED, std::move(result.message));
    case Status::kRemoteError:
      return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, std::move(result.message));
    case Status::kProtocolError:
      return RpcResult::Error(grpc::StatusCode::DATA_LOSS, std::move(result.message));
    case Status::kInternal:
      return RpcResult::Error(grpc::StatusCode::INTERNAL, std::move(result.message));
  }
  return RpcResult::Error(grpc::StatusCode::INTERNAL, "unknown model call status");
}

}  // namespace

RpcResult RpcResult::Error(grpc::StatusCode code, std::string message) {
  return RpcResult{code, std::move(message), nullptr};
}

NorthboundCore::NorthboundCore(NorthboundServices services, std::function<void()> tick)
    : services_(std::move(services)),
      tick_(std::move(tick)),
      steady_start_(std::chrono::steady_clock::now()),
      system_start_(std::chrono::system_clock::now()) {
  RefreshPublicSnapshots();
  RefreshProjectedSnapshots();
}

NorthboundCore::~NorthboundCore() = default;

MonitorAudioSample CalculateAudioEnergy(const hardware::MicrophoneTelemetry& telemetry,
                                        std::uint64_t sequence,
                                        std::chrono::system_clock::time_point captured_at) {
  long double sum_squares = 0.0;
  std::int32_t peak_sample = 0;
  bool clipping = false;
  for (const std::int16_t value : telemetry.pcm) {
    const std::int32_t magnitude = value == std::numeric_limits<std::int16_t>::min()
                                       ? 32768
                                       : std::abs(static_cast<std::int32_t>(value));
    peak_sample = std::max(peak_sample, magnitude);
    clipping = clipping || magnitude >= 32767;
    const long double normalized = static_cast<long double>(value) / 32768.0L;
    sum_squares += normalized * normalized;
  }
  const double rms = telemetry.pcm.empty()
                         ? 0.0
                         : std::sqrt(static_cast<double>(sum_squares / telemetry.pcm.size()));
  const double peak = static_cast<double>(peak_sample) / 32768.0;
  const double dbfs = rms > 0.0 ? std::max(-96.0, 20.0 * std::log10(rms)) : -96.0;
  return MonitorAudioSample{rms,
                            peak,
                            dbfs,
                            clipping,
                            sequence,
                            telemetry.source_timestamp_ns,
                            captured_at,
                            telemetry.sample_rate_hz,
                            telemetry.channels};
}

RpcResult NorthboundCore::Discover() {
  std::lock_guard<std::mutex> lock(public_snapshot_mutex_);
  if (!discovery_snapshot_ ||
      std::chrono::steady_clock::now() - discovery_snapshot_->sampled_at > kBodySnapshotMaxAge)
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "discovery snapshot unavailable");
  return {grpc::StatusCode::OK, {}, discovery_snapshot_->payload};
}

RpcResult NorthboundCore::BuildDiscover() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (services_.registry == nullptr) {
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "runtime discovery unavailable");
  }

  const hardware_runtime::Registry& registry = *services_.registry;
  std::size_t alive = 0;
  bool all_admitted = true;
  bool any_fault = false;
  bool any_active = false;
  bool any_unknown = false;
  const nlohmann::json exposed = services_.dispatcher != nullptr
                                     ? services_.dispatcher->Discover().at("primitives")
                                     : nlohmann::json::array();
  nlohmann::json devices = nlohmann::json::array();
  for (const hardware_runtime::Device& device : registry.devices()) {
    const hardware::BackendIdentity identity = device.identity();
    const hardware_runtime::DeviceHealth health = device.ReadHealth();
    const perception::body::DeviceStatus status = perception::body::ProjectDeviceStatus(device);
    if (health.alive) ++alive;
    all_admitted = all_admitted && health.admitted;
    any_fault = any_fault || status.state == "fault";
    any_active = any_active || status.state == "active" || status.state == "capturing" ||
                 status.state == "listening" || status.state == "buffering";
    any_unknown = any_unknown || status.state == "unknown";
    nlohmann::json primitives = nlohmann::json::array();
    for (const hardware_runtime::PrimitiveDeclaration& primitive : device.part().primitives) {
      const auto published = std::find_if(exposed.begin(), exposed.end(), [&](const auto& item) {
        return item.value("deviceId", "") == identity.device_id &&
               item.value("name", "") == primitive.name &&
               item.value("version", "") == primitive.version;
      });
      if (published == exposed.end()) continue;
      const hardware_runtime::PrimitiveSpec* spec =
          hardware_runtime::PrimitiveSpecOf(primitive.name, primitive.version);
      if (spec == nullptr) continue;
      nlohmann::json limits = nlohmann::json::object();
      for (const auto& [name, range] : primitive.limits) limits[name] = RangeJson(range);
      const bool advanced = spec->kind == hardware_runtime::PrimitiveKind::kAdvanced;
      std::string primitive_health = DeviceHealthLiteral(health);
      nlohmann::json dependencies = nlohmann::json::array();
      if (advanced && primitive.name == "speaker.speak") {
        const std::string model_health = ModelHealth(services_.models, primitive.model_id);
        dependencies.push_back({{"name", "tts"}, {"health", model_health}});
        dependencies.push_back({{"name", "speaker"}, {"health", DeviceHealthLiteral(health)}});
        if (model_health != "ready") primitive_health = model_health;
      } else if (advanced && primitive.name == "face.register") {
        const std::string model_health = ModelHealth(services_.models, primitive.model_id);
        dependencies.push_back({{"name", "face"}, {"health", model_health}});
        dependencies.push_back({{"name", "camera"}, {"health", DeviceHealthLiteral(health)}});
        if (primitive_health == "ready" || model_health == "unavailable") {
          primitive_health = model_health;
        }
      }
      nlohmann::json binding{
          {"name", primitive.name},
          {"version", primitive.version},
          {"description",
           primitive.description.empty() ? std::string(spec->description) : primitive.description},
          {"parameters", published->at("parameters")},
          {"kind", advanced ? "advanced" : "direct"},
          {"references", primitive.references},
          {"limits", std::move(limits)},
          {"health", primitive_health},
          {"dependencies", std::move(dependencies)},
          {"accepts", nlohmann::json::array()}};
      if (primitive.absolute_frame) binding["absoluteFrame"] = *primitive.absolute_frame;
      if (primitive.name == "speaker.play") {
        binding["accepts"] = nlohmann::json::array({"audio/wav"});
      }
      primitives.push_back(std::move(binding));
    }
    const auto implicit_stop = std::find_if(exposed.begin(), exposed.end(), [&](const auto& item) {
      return item.value("deviceId", "") == identity.device_id &&
             item.value("name", "") == "motion.stop" && item.value("version", "") == "1.0.0";
    });
    if (implicit_stop != exposed.end()) {
      const hardware_runtime::PrimitiveSpec* spec =
          hardware_runtime::PrimitiveSpecOf("motion.stop", "1.0.0");
      primitives.push_back({{"name", "motion.stop"},
                            {"version", "1.0.0"},
                            {"description", spec == nullptr ? "" : spec->description},
                            {"parameters", implicit_stop->at("parameters")},
                            {"kind", "direct"},
                            {"references", nlohmann::json::array()},
                            {"limits", nlohmann::json::object()},
                            {"health", DeviceHealthLiteral(health)},
                            {"dependencies", nlohmann::json::array()},
                            {"accepts", nlohmann::json::array()}});
    }
    const char* segment = perception::body::SegmentOfKind(device.part().kind);
    if (segment == nullptr) continue;
    const hardware_runtime::RobotIdentity* device_robot = registry.FindRobot(device.robot_id());
    if (device_robot == nullptr) continue;
    devices.push_back({
        {"id", identity.device_id},
        {"class", segment},
        {"part", device.part().part_id},
        {"description", device.part().description.value_or("")},
        {"state_uri", "robots/" + device_robot->name + "/devices/" + identity.device_id + "/state"},
        {"state", status.state},
        {"health", DeviceHealthLiteral(health)},
        {"primitives", std::move(primitives)},
    });
  }

  const std::size_t total = registry.devices().size();
  const bool ready = total > 0 && alive == total && all_admitted;
  const hardware_runtime::RobotIdentity& robot = registry.robot();
  const bool estop = registry.EstopLatched();
  const std::string robot_state = estop || any_fault ? "fault"
                                  : any_active       ? "active"
                                  : any_unknown      ? "unknown"
                                                     : "idle";
  nlohmann::json robot_primitives = nlohmann::json::array();
  for (const auto& primitive : exposed) {
    if (!primitive.value("deviceId", "").empty()) continue;
    robot_primitives.push_back({{"name", primitive.at("name")},
                                {"version", primitive.at("version")},
                                {"description", primitive.value("description", "")},
                                {"parameters", primitive.at("parameters")},
                                {"kind", "direct"},
                                {"references", nlohmann::json::array()},
                                {"limits", nlohmann::json::object()},
                                {"health", ready && !estop ? "ready" : "degraded"},
                                {"dependencies", nlohmann::json::array()},
                                {"accepts", nlohmann::json::array()}});
  }
  return {grpc::StatusCode::OK,
          {},
          {{"robot",
            {{"id", robot.id},
             {"name", robot.name},
             {"model", robot.model.value_or("")},
             {"description", robot.description.value_or("")},
             {"state", robot_state},
             {"health", ready && !estop ? "ready" : "degraded"},
             {"estop", estop},
             {"primitives", std::move(robot_primitives)}}},
           {"devices", std::move(devices)}}};
}

RpcResult NorthboundCore::GetStatus(const std::string& correlation_id) {
  nlohmann::json payload;
  {
    std::lock_guard<std::mutex> lock(public_snapshot_mutex_);
    if (!status_snapshot_ ||
        std::chrono::steady_clock::now() - status_snapshot_->sampled_at > kBodySnapshotMaxAge)
      return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "status snapshot unavailable");
    payload = status_snapshot_->payload;
  }
  const double uptime = Round1(
      std::chrono::duration<double>(std::chrono::steady_clock::now() - steady_start_).count());
  payload["correlation_id"] = correlation_id;
  payload["uptime_seconds"] = uptime;
  for (auto& subsystem : payload["subsystems"]) subsystem["uptime_seconds"] = uptime;
  return {grpc::StatusCode::OK, {}, std::move(payload)};
}

RpcResult NorthboundCore::BuildStatus() {
  std::lock_guard<std::mutex> lock(mutex_);
  const double uptime = Round1(
      std::chrono::duration<double>(std::chrono::steady_clock::now() - steady_start_).count());
  const bool composed = services_.registry != nullptr && services_.projection != nullptr &&
                        services_.dispatcher != nullptr;
  HealthState aggregate = composed ? HealthState::kReady : HealthState::kDegraded;
  std::vector<SubsystemStatus> subsystems{
      {"northbound", composed ? HealthState::kReady : HealthState::kDegraded, uptime}};
  std::size_t devices_total = 0;
  std::size_t devices_alive = 0;
  std::size_t devices_admitted = 0;
  if (services_.registry != nullptr) {
    devices_total = services_.registry->devices().size();
    for (const hardware_runtime::Device& device : services_.registry->devices()) {
      const hardware_runtime::DeviceHealth health = device.ReadHealth();
      if (health.alive) ++devices_alive;
      if (health.admitted) ++devices_admitted;
    }
    const bool healthy = devices_total > 0 && devices_alive == devices_total &&
                         devices_admitted == devices_total && !services_.registry->EstopLatched();
    subsystems.push_back(
        {"hardware", healthy ? HealthState::kReady : HealthState::kDegraded, uptime});
    if (!healthy) aggregate = HealthState::kDegraded;
  }
  std::optional<nlohmann::json> model_summary;
  if (services_.models != nullptr) {
    const std::vector<models::ModelHealth> health = services_.models->HealthAll();
    nlohmann::json model_items = nlohmann::json::array();
    for (const models::ModelConfig& configured : services_.models->ConfiguredAll()) {
      const std::optional<models::ModelHealth> item =
          configured.enabled ? services_.models->Health(configured.id) : std::nullopt;
      model_items.push_back(
          {{"id", configured.id},
           {"capability", std::string(models::CapabilityName(configured.capability))},
           {"health", configured.enabled && item ? ModelHealthLiteral(item->status)
                                                   : "unavailable"},
           {"message", !configured.enabled
                           ? "model disabled"
                           : item ? item->message : "health not yet available"}});
    }
    std::size_t checking = 0;
    std::size_t ready = 0;
    std::size_t degraded = 0;
    std::size_t unavailable = 0;
    for (const models::ModelHealth& item : health) {
      switch (item.status) {
        case models::HealthStatus::kChecking:
          ++checking;
          break;
        case models::HealthStatus::kReady:
          ++ready;
          break;
        case models::HealthStatus::kDegraded:
          ++degraded;
          break;
        case models::HealthStatus::kUnavailable:
          ++unavailable;
          break;
      }
    }
    const bool healthy = ready == health.size();
    model_summary = nlohmann::json{{"status", healthy ? "ready" : "degraded"},
                                   {"total", health.size()},
                                   {"ready", ready},
                                   {"checking", checking},
                                   {"degraded", degraded},
                                   {"unavailable", unavailable},
                                   {"items", std::move(model_items)}};
    subsystems.push_back(
        {"models", healthy ? HealthState::kReady : HealthState::kDegraded, uptime});
    if (!healthy) aggregate = HealthState::kDegraded;
  }
  nlohmann::json items = nlohmann::json::array();
  for (const auto& sub : subsystems) {
    items.push_back({{"name", sub.name},
                     {"state", StateLiteral(sub.state)},
                     {"uptime_seconds", Round1(sub.uptime_seconds)}});
  }
  nlohmann::json payload{
      {"robot_id", services_.registry != nullptr ? services_.registry->robot().id : ""},
      {"robot_name", services_.registry != nullptr ? services_.registry->robot().name : ""},
      {"status", StateLiteral(aggregate)},
      {"version", kVersion},
      {"started_at", StartedAt(system_start_)},
      {"uptime_seconds", uptime},
      {"correlation_id", ""},
      {"subsystems", std::move(items)},
      {"robots", services_.projection != nullptr ? services_.projection->BuildRobotOverviews()
                                                 : nlohmann::json::array()}};
  if (services_.registry != nullptr) {
    payload["devices_total"] = devices_total;
    payload["devices_alive"] = devices_alive;
    payload["devices_admitted"] = devices_admitted;
  }
  if (model_summary) payload["models"] = std::move(*model_summary);
  return {grpc::StatusCode::OK, {}, std::move(payload)};
}

RpcResult NorthboundCore::GetModelStatus(const std::string& model_id) {
  if (model_id.empty())
    return RpcResult::Error(grpc::StatusCode::INVALID_ARGUMENT, "model_id is required");
  if (services_.models == nullptr)
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "model runtime is unavailable");
  const auto configured = services_.models->Configured(model_id);
  if (!configured)
    return RpcResult::Error(grpc::StatusCode::NOT_FOUND, "model is not configured: " + model_id);
  const auto health = configured->enabled ? services_.models->Health(model_id) : std::nullopt;
  const auto checked_at = health ? health->checked_at : std::chrono::system_clock::time_point{};
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(checked_at.time_since_epoch());
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
      checked_at.time_since_epoch() - seconds);
  return {grpc::StatusCode::OK, {},
          {{"model_id", model_id},
           {"capability", std::string(models::CapabilityName(configured->capability))},
           {"enabled", configured->enabled},
           {"status", configured->enabled && health ? ModelHealthLiteral(health->status) : "unavailable"},
           {"checked_at_seconds", seconds.count()},
           {"checked_at_nanos", nanos.count()},
           {"message", !configured->enabled ? "model disabled" : health ? health->message : "health not yet available"}}};
}

RpcResult NorthboundCore::ListResources() {
  std::lock_guard<std::mutex> lock(public_snapshot_mutex_);
  if (!resource_list_snapshot_ ||
      std::chrono::steady_clock::now() - resource_list_snapshot_->sampled_at > kBodySnapshotMaxAge)
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "resource list snapshot unavailable");
  return {grpc::StatusCode::OK, {}, resource_list_snapshot_->payload};
}

RpcResult NorthboundCore::BuildResourceList() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (services_.resources != nullptr) {
    return {grpc::StatusCode::OK, {}, services_.resources->ListResources()};
  }
  if (services_.projection == nullptr) {
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "resource projection unavailable");
  }
  return {grpc::StatusCode::OK, {}, services_.projection->ListResources()};
}

void NorthboundCore::RefreshPublicSnapshots() {
  const auto sample = [this](auto build, std::optional<CachedResource>& destination) {
    try {
      RpcResult result = (this->*build)();
      if (!result.ok()) return;
      CachedResource next{std::chrono::steady_clock::now(), std::move(result.payload)};
      std::lock_guard<std::mutex> lock(public_snapshot_mutex_);
      destination = std::move(next);
    } catch (const std::exception&) {
      // The last validated result remains readable until its one-second freshness limit.
    }
  };
  sample(&NorthboundCore::BuildDiscover, discovery_snapshot_);
  sample(&NorthboundCore::BuildStatus, status_snapshot_);
  sample(&NorthboundCore::BuildResourceList, resource_list_snapshot_);
}

bool NorthboundCore::DeviceAliveSnapshot(const std::string& device_id) {
  std::lock_guard<std::mutex> lock(public_snapshot_mutex_);
  if (!discovery_snapshot_ ||
      std::chrono::steady_clock::now() - discovery_snapshot_->sampled_at > kBodySnapshotMaxAge)
    return false;
  for (const auto& device : discovery_snapshot_->payload.at("devices")) {
    if (device.value("id", "") == device_id)
      return device.value("health", "unavailable") != "unavailable";
  }
  return false;
}

RpcResult NorthboundCore::ReadResource(const std::string& uri) { return ReadResource(uri, false); }

RpcResult NorthboundCore::ListNavigationPoints(const std::string& robot_name) {
  if (services_.navigation_points == nullptr)
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "navigation point store unavailable");
  try {
    nlohmann::json points = nlohmann::json::array();
    for (const auto& point : services_.navigation_points->List(robot_name))
      points.push_back(NavigationPointJson(point));
    return {grpc::StatusCode::OK, {}, {{"points", std::move(points)}}};
  } catch (const store::StoreError& error) {
    return StoreFailure(error);
  }
}

RpcResult NorthboundCore::GetNavigationPoint(const std::string& robot_name,
                                             const std::string& name) {
  if (services_.navigation_points == nullptr)
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "navigation point store unavailable");
  try {
    return {grpc::StatusCode::OK,
            {},
            NavigationPointJson(services_.navigation_points->Get(robot_name, name))};
  } catch (const store::StoreError& error) {
    return StoreFailure(error);
  }
}

RpcResult NorthboundCore::PutNavigationPoint(const std::string& robot_name, const std::string& name,
                                             double x_m, double y_m, double yaw_rad) {
  if (services_.navigation_points == nullptr)
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "navigation point store unavailable");
  try {
    return {grpc::StatusCode::OK,
            {},
            NavigationPointJson(
                services_.navigation_points->Put(robot_name, {name, x_m, y_m, yaw_rad}))};
  } catch (const store::StoreError& error) {
    return StoreFailure(error);
  }
}

RpcResult NorthboundCore::DeleteNavigationPoint(const std::string& robot_name,
                                                const std::string& name) {
  if (services_.navigation_points == nullptr)
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "navigation point store unavailable");
  try {
    services_.navigation_points->Delete(robot_name, name);
    return {grpc::StatusCode::OK, {}, nlohmann::json::object()};
  } catch (const store::StoreError& error) {
    return StoreFailure(error);
  }
}

RpcResult NorthboundCore::ReloadNavigationPoints() {
  if (services_.navigation_points == nullptr)
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "navigation point store unavailable");
  try {
    return {grpc::StatusCode::OK, {}, {{"point_count", services_.navigation_points->Reload()}}};
  } catch (const store::StoreError& error) {
    return StoreFailure(error);
  }
}

RpcResult NorthboundCore::ReadResourceForWatch(const std::string& uri) {
  return ReadResource(uri, true);
}

RpcResult NorthboundCore::ReadResource(const std::string& uri, bool use_watch_cache) {
  if (uri.empty()) return RpcResult::Error(grpc::StatusCode::INVALID_ARGUMENT, "uri is required");
  const bool projected = IsBodyResource(uri) || IsSemanticResource(uri);
  if (projected &&
      (services_.resources == nullptr || !services_.resources->FindHistory(uri))) {
    if (services_.registry != nullptr && !HasKnownRobot(*services_.registry, uri))
      return RpcResult::Error(grpc::StatusCode::NOT_FOUND, "unknown robot resource: " + uri);
    if (IsBodyResource(uri) && services_.registry != nullptr &&
        !IsKnownBodyResource(*services_.registry, uri))
      return RpcResult::Error(grpc::StatusCode::NOT_FOUND, "unknown body resource: " + uri);
    std::lock_guard<std::mutex> body_lock(body_mutex_);
    const auto sampled = body_snapshots_.find(uri);
    if (sampled == body_snapshots_.end())
      return RpcResult::Error(IsBodyResource(uri) ? grpc::StatusCode::UNAVAILABLE
                                                 : grpc::StatusCode::NOT_FOUND,
                              "projected resource has no sampled state: " + uri);
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - sampled->second.sampled_at);
    if (age > kBodySnapshotMaxAge)
      return RpcResult::Error(grpc::StatusCode::UNAVAILABLE,
                              "body resource sample is stale: " + uri);
    RpcResult result{grpc::StatusCode::OK, {}, sampled->second.payload};
    result.sample_age = age;
    return result;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (services_.resources == nullptr && services_.projection == nullptr) {
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "resource projection unavailable");
  }
  try {
    const auto now = std::chrono::steady_clock::now();
    if (use_watch_cache) {
      const auto cached = watch_cache_.find(uri);
      if (cached != watch_cache_.end() && now - cached->second.sampled_at < kMinIntervalMs) {
        return {grpc::StatusCode::OK, {}, cached->second.payload};
      }
    }
    nlohmann::json payload = services_.resources != nullptr
                                 ? services_.resources->ReadResource(uri, !use_watch_cache)
                                 : services_.projection->BuildByUri(uri, !use_watch_cache);
    watch_cache_.insert_or_assign(uri, CachedResource{now, payload});
    return {grpc::StatusCode::OK, {}, std::move(payload)};
  } catch (const std::runtime_error& e) {
    return RpcResult::Error(grpc::StatusCode::NOT_FOUND, e.what());
  }
}

RpcResult NorthboundCore::QueryResourceHistory(const std::string& uri,
                                               const ResourceHistoryQuery& query) {
  if (uri.empty()) return RpcResult::Error(grpc::StatusCode::INVALID_ARGUMENT, "uri is required");
  if (services_.resources == nullptr) {
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "resource history unavailable");
  }
  const auto reader = services_.resources->FindHistory(uri);
  if (!reader) return RpcResult::Error(grpc::StatusCode::NOT_FOUND, "resource history not found");

  std::vector<perception::ProductPtr> products;
  perception::RetentionState retention;
  bool retention_gap = false;
  bool truncated = false;
  try {
    if (query.kind == ResourceHistoryQuery::Kind::kSequenceRange) {
      if (query.sequence == 0) {
        return RpcResult::Error(grpc::StatusCode::INVALID_ARGUMENT,
                                "sequence range first sequence must be non-zero");
      }
      if (query.limit == 0 || query.limit > kMaxHistoryRange) {
        return RpcResult::Error(grpc::StatusCode::INVALID_ARGUMENT,
                                "sequence range limit must be in [1, 1000]");
      }
      const perception::ProductRange result =
          reader->RangeBySequence(query.sequence, query.last_sequence, query.limit);
      products = result.products;
      retention = result.retention;
      retention_gap = result.retention_gap;
      truncated = result.truncated;
    } else {
      perception::ProductLookup result;
      switch (query.kind) {
        case ResourceHistoryQuery::Kind::kSequence:
          if (query.sequence == 0) {
            return RpcResult::Error(grpc::StatusCode::INVALID_ARGUMENT,
                                    "sequence must be non-zero");
          }
          result = reader->AtSequence(query.sequence);
          break;
        case ResourceHistoryQuery::Kind::kBefore:
          result = reader->Before(query.timestamp);
          break;
        case ResourceHistoryQuery::Kind::kAfter:
          result = reader->After(query.timestamp);
          break;
        case ResourceHistoryQuery::Kind::kNearest:
          result = reader->Nearest(query.timestamp);
          break;
        case ResourceHistoryQuery::Kind::kSequenceRange:
          break;
      }
      if (result.product) products.push_back(result.product);
      retention = result.retention;
      retention_gap = result.retention_gap;
    }
  } catch (const std::invalid_argument& error) {
    return RpcResult::Error(grpc::StatusCode::INVALID_ARGUMENT, error.what());
  }

  nlohmann::json encoded = nlohmann::json::array();
  for (const auto& product : products) encoded.push_back(ProductJson(product));
  const auto& declaration = reader->declaration();
  return {grpc::StatusCode::OK,
          {},
          {{"uri", declaration.uri},
           {"schema_uri", declaration.schema_uri},
           {"products", std::move(encoded)},
           {"retention", RetentionJson(retention)},
           {"retention_gap", retention_gap},
           {"truncated", truncated}}};
}

std::optional<perception::ProductLookup> NorthboundCore::LatestPublishedResource(
    const std::string& uri) const {
  if (services_.resources == nullptr) return std::nullopt;
  const auto reader = services_.resources->FindHistory(uri);
  if (!reader) return std::nullopt;
  return reader->Latest();
}

std::optional<perception::ProductWait> NorthboundCore::WaitForPublishedResource(
    const std::string& uri, std::uint64_t after_sequence,
    const std::function<bool()>& cancel_requested) const {
  if (services_.resources == nullptr) return std::nullopt;
  const auto reader = services_.resources->FindHistory(uri);
  if (!reader) return std::nullopt;
  return reader->WaitNewer(after_sequence, cancel_requested);
}

std::optional<perception::ProducerLease> NorthboundCore::AcquireResourceDemand(
    const std::string& uri) const {
  return services_.resources == nullptr ? std::nullopt : services_.resources->AcquireDemand(uri);
}

RpcResult NorthboundCore::CallPrimitive(const std::string& name, const std::string& version,
                                        const std::string& device_id, nlohmann::json arguments,
                                        const std::string& command_id) {
  if (services_.dispatcher == nullptr) {
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "primitive dispatcher unavailable");
  }
  return DispatchResult(services_.dispatcher->ExecuteCommand(name, version, device_id,
                                                             std::move(arguments), command_id));
}

RpcResult NorthboundCore::CancelPrimitive(const std::string& command_id) {
  if (services_.dispatcher == nullptr) {
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "primitive dispatcher unavailable");
  }
  return DispatchResult(services_.dispatcher->CancelCommand(command_id));
}

RpcResult NorthboundCore::InferModel(const std::string& model_id, models::Capability capability,
                                     nlohmann::json request,
                                     models::CancelRequested cancel_requested) {
  if (model_id.empty())
    return RpcResult::Error(grpc::StatusCode::INVALID_ARGUMENT, "model_id is required");
  if (services_.models == nullptr)
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "model runtime unavailable");
  switch (services_.models->Resolve(model_id, capability)) {
    case models::ModelReferenceState::kEnabled:
      break;
    case models::ModelReferenceState::kMissing:
      return RpcResult::Error(grpc::StatusCode::NOT_FOUND, "unknown model '" + model_id + "'");
    case models::ModelReferenceState::kDisabled:
      return RpcResult::Error(grpc::StatusCode::FAILED_PRECONDITION,
                              "model '" + model_id + "' is disabled");
    case models::ModelReferenceState::kWrongCapability:
      return RpcResult::Error(grpc::StatusCode::FAILED_PRECONDITION,
                              "model '" + model_id + "' does not provide " +
                                  std::string(models::CapabilityName(capability)));
  }
  models::CallResult result = services_.models->Call(model_id, request, cancel_requested);
  if (cancel_requested && cancel_requested())
    return RpcResult::Error(grpc::StatusCode::CANCELLED, "perception request was cancelled");
  return ModelCallResult(std::move(result));
}

void NorthboundCore::SampleCameraFrames() {
  if (services_.registry == nullptr) return;
  for (const hardware_runtime::Device& device : services_.registry->devices()) {
    if (device.identity().kind != hardware::DeviceKind::kCamera || !device.ReadHealth().alive) {
      continue;
    }
    const std::optional<hardware::CameraTelemetry> observed =
        services_.telemetry ? services_.telemetry->LatestCamera(device.identity().device_id)
                            : device.feeds().ReadCameraTelemetry();
    const std::size_t max_frame_bytes =
        device.part().camera_frame_limit_bytes;
    if (!observed || observed->width_px == 0 || observed->height_px == 0 ||
        observed->frame_id.empty() ||
        (observed->source_timestamp_ns && *observed->source_timestamp_ns < 0) ||
        observed->jpeg.size() < 4 || observed->jpeg.size() > max_frame_bytes ||
        observed->jpeg[0] != 0xff || observed->jpeg[1] != 0xd8 ||
        observed->jpeg[observed->jpeg.size() - 2] != 0xff || observed->jpeg.back() != 0xd9) {
      camera_frames_.erase(device.identity().device_id);
      continue;
    }
    CachedFrame next{observed->width_px, observed->height_px, std::chrono::system_clock::now(),
                     observed->source_timestamp_ns, observed->jpeg};
    std::deque<CachedFrame>& frames = camera_frames_[device.identity().device_id];
    const bool duplicate =
        !frames.empty() &&
        ((next.source_timestamp_ns && frames.back().source_timestamp_ns &&
          next.source_timestamp_ns == frames.back().source_timestamp_ns) ||
         (!next.source_timestamp_ns && !frames.back().source_timestamp_ns &&
          next.width_px == frames.back().width_px && next.height_px == frames.back().height_px &&
          next.jpeg == frames.back().jpeg));
    if (duplicate) continue;
    frames.push_back(std::move(next));
    while (frames.size() > 2) frames.pop_front();
  }
}

RpcResult NorthboundCore::CaptureFrames(const std::string& robot_name, const std::string& device_id,
                                        std::uint32_t frame_count) {
  if (robot_name.empty() || device_id.empty()) {
    return RpcResult::Error(grpc::StatusCode::INVALID_ARGUMENT,
                            "robot_name and device_id are required");
  }
  if (frame_count < 1 || frame_count > 2) {
    return RpcResult::Error(grpc::StatusCode::INVALID_ARGUMENT, "frame_count must be 1 or 2");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (services_.registry == nullptr || services_.registry->robot().name != robot_name) {
    return RpcResult::Error(grpc::StatusCode::NOT_FOUND, "robot_name is not bound to this EOS");
  }
  const hardware_runtime::Device* device = services_.registry->FindDevice(device_id);
  if (device == nullptr || device->identity().kind != hardware::DeviceKind::kCamera) {
    return RpcResult::Error(grpc::StatusCode::NOT_FOUND,
                            "camera device '" + device_id + "' is not configured");
  }
  if (!DeviceAliveSnapshot(device_id)) {
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE,
                            "camera device '" + device_id + "' is unhealthy");
  }
  const auto found = camera_frames_.find(device_id);
  if (found == camera_frames_.end() || found->second.size() < frame_count) {
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE,
                            "camera device has insufficient distinct frames");
  }
  nlohmann::json frames = nlohmann::json::array();
  const std::size_t start = found->second.size() - frame_count;
  for (std::size_t i = start; i < found->second.size(); ++i) {
    const CachedFrame& frame = found->second[i];
    if (std::chrono::system_clock::now() - frame.captured_at > std::chrono::seconds(2))
      return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "camera frame is stale");
    eos::lib::LogEventWithAttachment(NorthboundLogger(), spdlog::level::info,
                                     "vision_snapshot_frame_captured",
                                     {{"device_id", device_id},
                                      {"frame_index", i - start},
                                      {"width_px", frame.width_px},
                                      {"height_px", frame.height_px}},
                                     "vision", ".jpg", "image/jpeg", frame.jpeg);
    nlohmann::json value{{"jpeg", Base64Encode(frame.jpeg)},
                         {"widthPx", frame.width_px},
                         {"heightPx", frame.height_px},
                         {"capturedAt", CapturedAt(frame.captured_at)}};
    frames.push_back(std::move(value));
  }
  return {grpc::StatusCode::OK,
          {},
          {{"type", "vision.snapshot"},
           {"schema_version", "1.0"},
           {"deviceId", device_id},
           {"frames", std::move(frames)}}};
}

RpcResult NorthboundCore::ObserveObjects(const std::string& robot_name,
                                         const std::string& device_id,
                                         const std::string& model_id,
                                         std::optional<std::int64_t> source_timestamp_ns,
                                         models::CancelRequested cancel_requested) {
  constexpr auto kMaximumFrameAge = std::chrono::seconds(2);
  if (robot_name.empty() || device_id.empty() || model_id.empty()) {
    return RpcResult::Error(grpc::StatusCode::INVALID_ARGUMENT,
                            "robot_name, device_id and model_id are required");
  }
  if (source_timestamp_ns && *source_timestamp_ns <= 0) {
    return RpcResult::Error(grpc::StatusCode::INVALID_ARGUMENT,
                            "source_timestamp_ns must be positive");
  }
  if (services_.registry == nullptr || services_.registry->robot().name != robot_name) {
    return RpcResult::Error(grpc::StatusCode::NOT_FOUND, "robot_name is not bound to this EOS");
  }
  const hardware_runtime::Device* device = services_.registry->FindDevice(device_id);
  if (device == nullptr || device->identity().kind != hardware::DeviceKind::kCamera) {
    return RpcResult::Error(grpc::StatusCode::NOT_FOUND,
                            "camera device '" + device_id + "' is not configured");
  }
  if (!DeviceAliveSnapshot(device_id)) {
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE,
                            "camera device '" + device_id + "' is unhealthy");
  }
  if (services_.models == nullptr) {
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "model runtime unavailable");
  }
  switch (services_.models->Resolve(model_id, models::Capability::kDetection)) {
    case models::ModelReferenceState::kEnabled:
      break;
    case models::ModelReferenceState::kMissing:
      return RpcResult::Error(grpc::StatusCode::NOT_FOUND, "unknown model '" + model_id + "'");
    case models::ModelReferenceState::kDisabled:
      return RpcResult::Error(grpc::StatusCode::FAILED_PRECONDITION,
                              "model '" + model_id + "' is disabled");
    case models::ModelReferenceState::kWrongCapability:
      return RpcResult::Error(grpc::StatusCode::FAILED_PRECONDITION,
                              "model '" + model_id + "' does not provide detection");
  }
  std::optional<hardware::CameraTelemetry> frame;
  if (services_.telemetry) {
    const auto snapshot = source_timestamp_ns
                              ? services_.telemetry->FrameAt(device_id, *source_timestamp_ns)
                              : services_.telemetry->SnapshotCamera(device_id);
    if (!snapshot || std::chrono::steady_clock::now() - snapshot->received_at > kMaximumFrameAge) {
      return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "camera frame is unavailable or stale");
    }
    frame = snapshot->telemetry;
  } else {
    if (source_timestamp_ns) {
      return RpcResult::Error(grpc::StatusCode::UNAVAILABLE,
                              "exact camera frame lookup requires telemetry history");
    }
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE,
                            "camera telemetry history is unavailable");
  }
  const std::size_t max_frame_bytes =
      device->part().camera_frame_limit_bytes;
  if (!frame || frame->width_px == 0 || frame->height_px == 0 || frame->frame_id.empty() ||
      !frame->source_timestamp_ns || *frame->source_timestamp_ns <= 0 ||
      frame->jpeg.size() < 4 || frame->jpeg.size() > max_frame_bytes ||
      frame->jpeg[0] != 0xff || frame->jpeg[1] != 0xd8 ||
      frame->jpeg[frame->jpeg.size() - 2] != 0xff || frame->jpeg.back() != 0xd9) {
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE,
                            "camera frame has invalid metadata or JPEG payload");
  }
  if (source_timestamp_ns && frame->source_timestamp_ns != source_timestamp_ns) {
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE,
                            "requested camera frame is no longer available");
  }
  const std::string jpeg_b64 = Base64Encode(frame->jpeg);
  const RpcResult inferred = InferModel(
      model_id, models::Capability::kDetection,
      {{"input", {{"data_b64", jpeg_b64}, {"filename", "camera.jpg"},
                  {"media_type", "image/jpeg"}}}},
      std::move(cancel_requested));
  if (!inferred.ok()) return inferred;
  if (!inferred.payload.is_object() || !inferred.payload.contains("objects") ||
      !inferred.payload["objects"].is_array()) {
    return RpcResult::Error(grpc::StatusCode::DATA_LOSS, "model returned invalid objects");
  }
  return {grpc::StatusCode::OK,
          {},
          {{"deviceId", device_id},
           {"modelId", model_id},
           {"frameId", frame->frame_id},
           {"sourceTimestampNs", std::to_string(*frame->source_timestamp_ns)},
           {"widthPx", frame->width_px},
           {"heightPx", frame->height_px},
           {"jpeg", jpeg_b64},
           {"objects", inferred.payload["objects"]}}};
}

RpcResult NorthboundCore::ValidateMonitor(const std::string& camera_device_id,
                                          const std::string& microphone_device_id) {
  if (camera_device_id.empty()) {
    return RpcResult::Error(grpc::StatusCode::INVALID_ARGUMENT,
                            "camera_device_id is required");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (services_.registry == nullptr) {
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "runtime telemetry unavailable");
  }
  const hardware_runtime::Device* camera = services_.registry->FindDevice(camera_device_id);
  if (camera == nullptr || camera->identity().kind != hardware::DeviceKind::kCamera) {
    return RpcResult::Error(grpc::StatusCode::NOT_FOUND,
                            "camera device '" + camera_device_id + "' is not configured");
  }
  const hardware_runtime::Device* microphone = nullptr;
  if (!microphone_device_id.empty()) {
    microphone = services_.registry->FindDevice(microphone_device_id);
    if (microphone == nullptr || microphone->identity().kind != hardware::DeviceKind::kMicrophone) {
      return RpcResult::Error(grpc::StatusCode::NOT_FOUND,
                              "microphone device '" + microphone_device_id + "' is not configured");
    }
  }
  if (!DeviceAliveSnapshot(camera_device_id) ||
      (microphone != nullptr && !DeviceAliveSnapshot(microphone_device_id))) {
    return RpcResult::Error(grpc::StatusCode::UNAVAILABLE, "selected monitor device is unhealthy");
  }
  return {grpc::StatusCode::OK, {}, nlohmann::json::object()};
}

MonitorSample NorthboundCore::ReadMonitor(const std::string& camera_device_id,
                                          const std::string& microphone_device_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  MonitorSample sample;
  if (services_.registry == nullptr) return sample;
  const auto now = std::chrono::system_clock::now();
  if (const hardware_runtime::Device* camera = services_.registry->FindDevice(camera_device_id);
      camera != nullptr && camera->identity().kind == hardware::DeviceKind::kCamera &&
      DeviceAliveSnapshot(camera_device_id)) {
    const auto snapshot = services_.telemetry
                              ? services_.telemetry->SnapshotCamera(camera_device_id)
                              : std::optional<hardware_runtime::TelemetryHub::CameraSnapshot>{};
    const auto observed =
        services_.telemetry
            ? (snapshot ? std::optional<hardware::CameraTelemetry>(snapshot->telemetry)
                        : std::nullopt)
            : std::optional<hardware::CameraTelemetry>{};
    if (observed && observed->width_px > 0 && observed->height_px > 0 &&
        observed->jpeg.size() >= 4 && observed->jpeg.front() == 0xff && observed->jpeg[1] == 0xd8 &&
        observed->jpeg[observed->jpeg.size() - 2] == 0xff && observed->jpeg.back() == 0xd9) {
      sample.camera = MonitorCameraSample{
          observed->width_px,
          observed->height_px,
          snapshot ? snapshot->generation : ++monitor_camera_sequences_[camera_device_id],
          observed->source_timestamp_ns,
          now,
          observed->jpeg};
    }
  }
  if (const hardware_runtime::Device* microphone =
          services_.registry->FindDevice(microphone_device_id);
      microphone != nullptr && microphone->identity().kind == hardware::DeviceKind::kMicrophone &&
      DeviceAliveSnapshot(microphone_device_id)) {
    const auto snapshot = services_.telemetry
                              ? services_.telemetry->SnapshotMicrophone(microphone_device_id)
                              : std::optional<hardware_runtime::TelemetryHub::MicrophoneSnapshot>{};
    const auto observed =
        services_.telemetry
            ? (snapshot ? std::optional<hardware::MicrophoneTelemetry>(snapshot->telemetry)
                        : std::nullopt)
            : std::optional<hardware::MicrophoneTelemetry>{};
    if (observed && observed->sample_rate_hz > 0 && observed->channels > 0 &&
        !observed->pcm.empty()) {
      sample.audio = CalculateAudioEnergy(
          *observed,
          snapshot ? snapshot->generation : ++monitor_audio_sequences_[microphone_device_id], now);
    }
  }
  return sample;
}

void NorthboundCore::Tick() {
  if (tick_) tick_();
  RefreshPublicSnapshots();
  RefreshProjectedSnapshots();
  std::lock_guard<std::mutex> lock(mutex_);
  SampleCameraFrames();
}

void NorthboundCore::RefreshProjectedSnapshots() {
  std::map<std::string, CachedResource> sampled_body;
  std::set<std::string> invalid_body;
  if (services_.projection != nullptr) {
    // Driver reads happen on the runtime cadence, never on a public body read.
    try {
      for (const auto& entry : services_.projection->ListResources()) {
        const std::string uri = entry.value("uri", "");
        if (!IsBodyResource(uri) && !IsSemanticResource(uri)) continue;
        try {
          nlohmann::json payload = services_.projection->BuildByUri(uri, false);
          if (payload.contains("fresh") && payload.at("fresh") == false) {
            invalid_body.insert(uri);
            continue;
          }
          sampled_body.insert_or_assign(
              uri, CachedResource{std::chrono::steady_clock::now(), std::move(payload)});
        } catch (const std::exception&) {
          // Keep the previous sample until its age forces an unavailable result.
        }
      }
    } catch (const std::exception&) {
      // An unavailable inventory cannot stop the runtime tick thread.
    }
  }
  {
    std::lock_guard<std::mutex> body_lock(body_mutex_);
    for (const auto& uri : invalid_body) body_snapshots_.erase(uri);
    for (auto& [uri, snapshot] : sampled_body)
      body_snapshots_.insert_or_assign(uri, std::move(snapshot));
  }
}

grpc::Status NorthboundGrpcService::Discover(grpc::ServerContext* context,
                                             const google::protobuf::Empty*,
                                             island::eos::v1::DiscoverResponse* response) {
  return HandleGrpcRequest(context, "Discover", [this, response](const std::string&) {
    const RpcResult result = core_.Discover();
    if (!result.ok()) return AsStatus(result);
    return JsonToMessage(result.payload, response);
  });
}

grpc::Status NorthboundGrpcService::GetStatus(grpc::ServerContext* context,
                                              const google::protobuf::Empty*,
                                              google::protobuf::Struct* response) {
  return HandleGrpcRequest(context, "GetStatus",
                           [this, response](const std::string& correlation_id) {
                             const RpcResult result = core_.GetStatus(correlation_id);
                             if (!result.ok()) return AsStatus(result);
                             return JsonToMessage(result.payload, response);
                           });
}

namespace {
void FillModelStatus(const nlohmann::json& payload, island::eos::v1::ModelStatus* response) {
  response->set_model_id(payload.at("model_id").get<std::string>());
  response->set_capability(payload.at("capability").get<std::string>());
  response->set_enabled(payload.at("enabled").get<bool>());
  response->set_status(payload.at("status").get<std::string>());
  response->mutable_checked_at()->set_seconds(payload.at("checked_at_seconds").get<std::int64_t>());
  response->mutable_checked_at()->set_nanos(payload.at("checked_at_nanos").get<std::int32_t>());
  response->set_message(payload.at("message").get<std::string>());
}
}  // namespace

grpc::Status NorthboundGrpcService::GetModelStatus(
    grpc::ServerContext* context, const island::eos::v1::ModelStatusRequest* request,
    island::eos::v1::ModelStatus* response) {
  return HandleGrpcRequest(context, "GetModelStatus", [this, request, response](const std::string&) {
    const RpcResult result = core_.GetModelStatus(request->model_id());
    if (!result.ok()) return AsStatus(result);
    FillModelStatus(result.payload, response);
    return grpc::Status::OK;
  });
}

grpc::Status NorthboundGrpcService::WatchModelStatus(
    grpc::ServerContext* context, const island::eos::v1::ModelStatusRequest* request,
    grpc::ServerWriter<island::eos::v1::ModelStatus>* writer) {
  return HandleGrpcRequest(context, "WatchModelStatus", [this, context, request, writer](const std::string&) {
    std::string previous;
    while (!context->IsCancelled()) {
      const RpcResult result = core_.GetModelStatus(request->model_id());
      if (!result.ok()) return AsStatus(result);
      const std::string current = result.payload.dump();
      if (current != previous) {
        island::eos::v1::ModelStatus response;
        FillModelStatus(result.payload, &response);
        if (!writer->Write(response)) return grpc::Status::OK;
        previous = current;
      }
      for (int i = 0; i < 5 && !context->IsCancelled(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return grpc::Status::OK;
  });
}

grpc::Status NorthboundGrpcService::ListResources(grpc::ServerContext* context,
                                                  const google::protobuf::Empty*,
                                                  google::protobuf::ListValue* response) {
  return HandleGrpcRequest(context, "ListResources", [this, response](const std::string&) {
    const RpcResult result = core_.ListResources();
    if (!result.ok()) return AsStatus(result);
    return JsonToMessage(result.payload, response);
  });
}

grpc::Status NorthboundGrpcService::ReadResource(grpc::ServerContext* context,
                                                 const island::eos::v1::ResourceRequest* request,
                                                 google::protobuf::Struct* response) {
  return HandleGrpcRequest(
      context, "ReadResource", [this, context, request, response](const std::string&) {
        const RpcResult result = core_.ReadResource(request->uri());
        if (!result.ok()) return AsStatus(result);
        if (result.sample_age)
          context->AddInitialMetadata("eos-sample-age-ms",
                                      std::to_string(result.sample_age->count()));
        return JsonToMessage(result.payload, response);
      });
}

grpc::Status NorthboundGrpcService::ListNavigationPoints(
    grpc::ServerContext* context, const island::eos::v1::ListNavigationPointsRequest* request,
    island::eos::v1::ListNavigationPointsResponse* response) {
  return HandleGrpcRequest(
      context, "ListNavigationPoints", [this, request, response](const std::string&) {
        const RpcResult result = core_.ListNavigationPoints(request->robot_name());
        if (!result.ok()) return AsStatus(result);
        return JsonToMessage(result.payload, response);
      });
}

grpc::Status NorthboundGrpcService::GetNavigationPoint(
    grpc::ServerContext* context, const island::eos::v1::GetNavigationPointRequest* request,
    island::eos::v1::NavigationPoint* response) {
  return HandleGrpcRequest(
      context, "GetNavigationPoint", [this, request, response](const std::string&) {
        const RpcResult result = core_.GetNavigationPoint(request->robot_name(), request->name());
        if (!result.ok()) return AsStatus(result);
        return JsonToMessage(result.payload, response);
      });
}

grpc::Status NorthboundGrpcService::PutNavigationPoint(
    grpc::ServerContext* context, const island::eos::v1::PutNavigationPointRequest* request,
    island::eos::v1::NavigationPoint* response) {
  return HandleGrpcRequest(
      context, "PutNavigationPoint", [this, request, response](const std::string&) {
        if (!request->has_x_m() || !request->has_y_m() || !request->has_yaw_rad()) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                              "x_m, y_m, and yaw_rad are required");
        }
        const RpcResult result =
            core_.PutNavigationPoint(request->robot_name(), request->name(), request->x_m(),
                                     request->y_m(), request->yaw_rad());
        if (!result.ok()) return AsStatus(result);
        return JsonToMessage(result.payload, response);
      });
}

grpc::Status NorthboundGrpcService::DeleteNavigationPoint(
    grpc::ServerContext* context, const island::eos::v1::GetNavigationPointRequest* request,
    google::protobuf::Empty*) {
  return HandleGrpcRequest(context, "DeleteNavigationPoint", [this, request](const std::string&) {
    return AsStatus(core_.DeleteNavigationPoint(request->robot_name(), request->name()));
  });
}

grpc::Status NorthboundGrpcService::ReloadNavigationPoints(
    grpc::ServerContext* context, const google::protobuf::Empty*,
    island::eos::v1::ReloadNavigationPointsResponse* response) {
  return HandleGrpcRequest(context, "ReloadNavigationPoints", [this, response](const std::string&) {
    const RpcResult result = core_.ReloadNavigationPoints();
    if (!result.ok()) return AsStatus(result);
    return JsonToMessage(result.payload, response);
  });
}

grpc::Status NorthboundGrpcService::QueryResourceHistory(
    grpc::ServerContext* context, const island::eos::v1::QueryResourceHistoryRequest* request,
    island::eos::v1::QueryResourceHistoryResponse* response) {
  return HandleGrpcRequest(
      context, "QueryResourceHistory", [this, request, response](const std::string&) {
        ResourceHistoryQuery query;
        switch (request->query_case()) {
          case island::eos::v1::QueryResourceHistoryRequest::kSequence:
            query.kind = ResourceHistoryQuery::Kind::kSequence;
            query.sequence = request->sequence();
            break;
          case island::eos::v1::QueryResourceHistoryRequest::kBefore:
          case island::eos::v1::QueryResourceHistoryRequest::kAfter:
          case island::eos::v1::QueryResourceHistoryRequest::kNearest: {
            const google::protobuf::Timestamp* timestamp = nullptr;
            if (request->query_case() == island::eos::v1::QueryResourceHistoryRequest::kBefore) {
              query.kind = ResourceHistoryQuery::Kind::kBefore;
              timestamp = &request->before();
            } else if (request->query_case() ==
                       island::eos::v1::QueryResourceHistoryRequest::kAfter) {
              query.kind = ResourceHistoryQuery::Kind::kAfter;
              timestamp = &request->after();
            } else {
              query.kind = ResourceHistoryQuery::Kind::kNearest;
              timestamp = &request->nearest();
            }
            const auto converted = ChronoTimestamp(*timestamp);
            if (!converted) {
              return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "timestamp is invalid");
            }
            query.timestamp = *converted;
            break;
          }
          case island::eos::v1::QueryResourceHistoryRequest::kSequenceRange:
            query.kind = ResourceHistoryQuery::Kind::kSequenceRange;
            query.sequence = request->sequence_range().first_sequence();
            query.last_sequence = request->sequence_range().last_sequence();
            query.limit = request->sequence_range().limit();
            break;
          case island::eos::v1::QueryResourceHistoryRequest::QUERY_NOT_SET:
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "one history query selector is required");
        }
        const RpcResult result = core_.QueryResourceHistory(request->uri(), query);
        if (!result.ok()) return AsStatus(result);
        return JsonToMessage(result.payload, response);
      });
}

grpc::Status NorthboundGrpcService::WatchResource(
    grpc::ServerContext* context, const island::eos::v1::ResourceRequest* request,
    grpc::ServerWriter<island::eos::v1::ResourceChanged>* writer) {
  return HandleGrpcRequest(
      context, "WatchResource", [this, context, request, writer](const std::string&) {
        const auto started = std::chrono::steady_clock::now();
        std::size_t changes = 0;
        eos::lib::LogEvent(NorthboundLogger(), spdlog::level::info, "resource_watch_opened",
                           {{"uri", request->uri()}});
        std::optional<perception::ProducerLease> demand;
        try {
          demand = core_.AcquireResourceDemand(request->uri());
        } catch (const std::exception& error) {
          return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                              "resource producer failed to start: " + std::string(error.what()));
        } catch (...) {
          return grpc::Status(grpc::StatusCode::UNAVAILABLE, "resource producer failed to start");
        }
        auto published = core_.LatestPublishedResource(request->uri());
        if (published) {
          if (!published->product) {
            const auto first = core_.WaitForPublishedResource(
                request->uri(), 0, [context] { return context->IsCancelled(); });
            if (context->IsCancelled() ||
                (first && first->status == perception::WaitStatus::kCancelled)) {
              return grpc::Status::OK;
            }
            if (!first || first->status != perception::WaitStatus::kProduct || !first->product)
              return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                  "resource has no published product: " + request->uri());
            published->product = first->product;
          }
          island::eos::v1::ResourceChanged initial;
          initial.set_uri(request->uri());
          grpc::Status encoded =
              JsonToMessage(published->product->value, initial.mutable_resource());
          if (!encoded.ok()) return encoded;
          if (!writer->Write(initial)) return grpc::Status::OK;
          ++changes;
          std::uint64_t sequence = published->product->sequence;
          while (!context->IsCancelled()) {
            const auto next = core_.WaitForPublishedResource(
                request->uri(), sequence, [context] { return context->IsCancelled(); });
            if (!next || next->status == perception::WaitStatus::kClosed ||
                next->status == perception::WaitStatus::kCancelled) {
              break;
            }
            if (!next->product) continue;
            sequence = next->product->sequence;
            island::eos::v1::ResourceChanged changed;
            changed.set_uri(request->uri());
            encoded = JsonToMessage(next->product->value, changed.mutable_resource());
            if (!encoded.ok()) return encoded;
            if (!writer->Write(changed)) break;
            ++changes;
          }
          eos::lib::LogEvent(NorthboundLogger(), spdlog::level::info, "resource_watch_closed",
                             {{"uri", request->uri()},
                              {"cancelled", context->IsCancelled()},
                              {"changes", changes},
                              {"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::steady_clock::now() - started)
                                                  .count()}});
          return grpc::Status::OK;
        }
        RpcResult baseline = core_.ReadResourceForWatch(request->uri());
        if (!baseline.ok()) {
          eos::lib::LogEvent(NorthboundLogger(), spdlog::level::warn, "resource_watch_failed",
                             {{"uri", request->uri()},
                              {"status", static_cast<int>(baseline.code)},
                              {"stage", "baseline"}});
          return AsStatus(baseline);
        }
        island::eos::v1::ResourceChanged initial;
        initial.set_uri(request->uri());
        grpc::Status encoded = JsonToMessage(baseline.payload, initial.mutable_resource());
        if (!encoded.ok()) return encoded;
        if (!writer->Write(initial)) return grpc::Status::OK;
        ++changes;
        ChangeTracker tracker(baseline.payload);
        while (!context->IsCancelled()) {
          std::this_thread::sleep_for(kMinIntervalMs);
          RpcResult current = core_.ReadResourceForWatch(request->uri());
          if (!current.ok()) {
            eos::lib::LogEvent(NorthboundLogger(), spdlog::level::warn, "resource_watch_failed",
                               {{"uri", request->uri()},
                                {"status", static_cast<int>(current.code)},
                                {"stage", "read"},
                                {"changes", changes}});
            return AsStatus(current);
          }
          const auto sequence = tracker.Observe(current.payload, std::chrono::steady_clock::now());
          if (sequence) {
            island::eos::v1::ResourceChanged changed;
            changed.set_uri(request->uri());
            encoded = JsonToMessage(current.payload, changed.mutable_resource());
            if (!encoded.ok()) return encoded;
            if (!writer->Write(changed)) {
              eos::lib::LogEvent(NorthboundLogger(), spdlog::level::warn,
                                 "resource_watch_write_failed",
                                 {{"uri", request->uri()}, {"changes", changes}});
              break;
            }
            ++changes;
            eos::lib::LogEvent(NorthboundLogger(), spdlog::level::debug, "resource_watch_changed",
                               {{"uri", request->uri()}, {"sequence", *sequence}});
          }
        }
        eos::lib::LogEvent(NorthboundLogger(), spdlog::level::info, "resource_watch_closed",
                           {{"uri", request->uri()},
                            {"cancelled", context->IsCancelled()},
                            {"changes", changes},
                            {"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - started)
                                                .count()}});
        return grpc::Status::OK;
      });
}

grpc::Status NorthboundGrpcService::CallPrimitive(
    grpc::ServerContext* context, const island::eos::v1::CallPrimitiveRequest* request,
    google::protobuf::Struct* response) {
  return HandleGrpcRequest(context, "CallPrimitive", [this, request, response](const std::string&) {
    const RpcResult result =
        core_.CallPrimitive(request->name(), request->version(), request->device_id(),
                            StructToJson(request->arguments()), request->command_id());
    if (!result.ok()) return AsStatus(result);
    return JsonToMessage(result.payload, response);
  });
}

grpc::Status NorthboundGrpcService::CancelPrimitive(
    grpc::ServerContext* context, const island::eos::v1::PrimitiveCommandRequest* request,
    google::protobuf::Struct* response) {
  return HandleGrpcRequest(context, "CancelPrimitive",
                           [this, request, response](const std::string&) {
                             const RpcResult result = core_.CancelPrimitive(request->command_id());
                             if (!result.ok()) return AsStatus(result);
                             return JsonToMessage(result.payload, response);
                           });
}

grpc::Status NorthboundGrpcService::CaptureFrames(
    grpc::ServerContext* context, const island::eos::v1::CaptureFramesRequest* request,
    island::eos::v1::CaptureFramesResponse* response) {
  return HandleGrpcRequest(context, "CaptureFrames", [this, request, response](const std::string&) {
    const RpcResult result =
        core_.CaptureFrames(request->robot_name(), request->device_id(), request->frame_count());
    if (!result.ok()) return AsStatus(result);
    return JsonToMessage(result.payload, response);
  });
}

grpc::Status NorthboundGrpcService::ObserveObjects(
    grpc::ServerContext* context, const island::eos::v1::ObserveObjectsRequest* request,
    island::eos::v1::ObserveObjectsResponse* response) {
  return HandleGrpcRequest(context, "ObserveObjects", [this, context, request, response](const std::string&) {
    const RpcResult result = core_.ObserveObjects(
        request->robot_name(), request->device_id(), request->model_id(),
        request->has_source_timestamp_ns()
            ? std::optional<std::int64_t>(request->source_timestamp_ns())
            : std::nullopt,
        [context] { return context->IsCancelled(); });
    if (!result.ok()) return AsStatus(result);
    return JsonToMessage(result.payload, response);
  });
}

grpc::Status NorthboundGrpcService::InferVad(grpc::ServerContext* context,
                                             const island::eos::v1::InferVadRequest* request,
                                             island::eos::v1::InferVadResponse* response) {
  return HandleGrpcRequest(
      context, "InferVad", [this, context, request, response](const std::string&) {
        nlohmann::json input{{"input", AudioJson(request->input())}};
        if (request->has_threshold()) input["threshold"] = request->threshold();
        const RpcResult result =
            core_.InferModel(request->model_id(), models::Capability::kVad, std::move(input),
                             [context] { return context->IsCancelled(); });
        return PerceptionResponse(result, response);
      });
}

grpc::Status NorthboundGrpcService::DetectHumans(
    grpc::ServerContext* context, const island::eos::v1::DetectHumansRequest* request,
    island::eos::v1::DetectHumansResponse* response) {
  return HandleGrpcRequest(
      context, "DetectHumans", [this, context, request, response](const std::string&) {
        nlohmann::json input{{"input", MediaJson(request->input())}};
        if (request->has_depth_input()) input["depth_input"] = MediaJson(request->depth_input());
        if (request->has_depth_scale()) input["depth_scale"] = request->depth_scale();
        if (request->has_confidence_threshold()) input["conf"] = request->confidence_threshold();
        const RpcResult result =
            core_.InferModel(request->model_id(), models::Capability::kHuman, std::move(input),
                             [context] { return context->IsCancelled(); });
        return PerceptionResponse(result, response);
      });
}

grpc::Status NorthboundGrpcService::EstimateHumanPose(
    grpc::ServerContext* context, const island::eos::v1::EstimateHumanPoseRequest* request,
    island::eos::v1::EstimateHumanPoseResponse* response) {
  return HandleGrpcRequest(
      context, "EstimateHumanPose", [this, context, request, response](const std::string&) {
        nlohmann::json input{{"input", MediaJson(request->input())}};
        if (request->has_depth_input()) input["depth_input"] = MediaJson(request->depth_input());
        if (request->has_depth_scale()) input["depth_scale"] = request->depth_scale();
        if (!request->persons().empty()) {
          input["persons"] = nlohmann::json::array();
          for (const auto& person : request->persons())
            input["persons"].push_back(PersonJson(person));
        }
        if (!request->furniture().empty()) {
          input["furniture"] = nlohmann::json::array();
          for (const auto& furniture : request->furniture()) {
            input["furniture"].push_back(
                {{"bbox", BboxJson(furniture.bbox())}, {"class_name", furniture.class_name()}});
          }
        }
        if (request->has_confidence_threshold()) input["conf"] = request->confidence_threshold();
        if (request->has_keypoint_confidence_threshold())
          input["kpt_conf"] = request->keypoint_confidence_threshold();
        const RpcResult result =
            core_.InferModel(request->model_id(), models::Capability::kHumanPose, std::move(input),
                             [context] { return context->IsCancelled(); });
        return PerceptionResponse(result, response);
      });
}

grpc::Status NorthboundGrpcService::DetectFaces(grpc::ServerContext* context,
                                                const island::eos::v1::DetectFacesRequest* request,
                                                island::eos::v1::DetectFacesResponse* response) {
  return HandleGrpcRequest(
      context, "DetectFaces", [this, context, request, response](const std::string&) {
        nlohmann::json input{{"operation", "recognize"}, {"input", MediaJson(request->input())}};
        if (request->has_return_image()) input["return_image"] = request->return_image();
        const RpcResult result =
            core_.InferModel(request->model_id(), models::Capability::kFace, std::move(input),
                             [context] { return context->IsCancelled(); });
        return PerceptionResponse(result, response);
      });
}

grpc::Status NorthboundGrpcService::GetPerceptionRoles(
    grpc::ServerContext* context, const google::protobuf::Empty*,
    island::eos::v1::PerceptionRolesResponse* response) {
  return HandleGrpcRequest(context, "GetPerceptionRoles", [this, response](const std::string&) {
    for (const auto& configured : core_.services_.perception_roles) {
      if (configured.model_id.empty()) continue;
      auto* role = response->add_roles();
      role->set_role(configured.role);
      role->set_model_id(configured.model_id);
      role->set_continuous(configured.continuous);
      role->set_available(!configured.model_id.empty() &&
                          ModelHealth(core_.services_.models, configured.model_id) == "ready");
    }
    if (!core_.services_.perception_camera_device_id.empty())
      response->set_camera_device_id(core_.services_.perception_camera_device_id);
    return grpc::Status::OK;
  });
}

grpc::Status NorthboundGrpcService::WatchMonitor(
    grpc::ServerContext* context, const island::eos::v1::MonitorRequest* request,
    grpc::ServerWriter<island::eos::v1::MonitorEvent>* writer) {
  return HandleGrpcRequest(
      context, "WatchMonitor", [this, context, request, writer](const std::string&) {
        if (request->max_camera_fps() > 30) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                              "max_camera_fps must be between 1 and 30");
        }
        if (request->version() != 1) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                                        "monitor version must be 1");
        if (request->detection_source() != "" && request->detection_source() != "human" &&
            request->detection_source() != "object")
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                              "detection_source must be human or object");
        const RpcResult validation = core_.ValidateMonitor(request->camera_device_id(),
                                                            request->microphone_device_id());
        if (!validation.ok()) return AsStatus(validation);
        std::optional<perception::ProducerLease> detection_demand;
        const bool object_detections = request->detection_source() == "object";
        const std::string detection_camera = object_detections
            ? request->camera_device_id()
            : core_.services_.perception_camera_device_id;
        if (request->include_detections() &&
            request->camera_device_id() == detection_camera &&
            core_.services_.registry != nullptr) {
          const auto* camera = core_.services_.registry->FindDevice(request->camera_device_id());
          if (camera != nullptr) {
            for (const auto& robot : core_.services_.registry->robots()) {
              if (robot.id == camera->robot_id()) {
                detection_demand = core_.AcquireResourceDemand(
                    object_detections
                        ? "robots/" + robot.name + "/cameras/" +
                              request->camera_device_id() + "/detections/state"
                        : "robots/" + robot.name + "/detection/state");
                break;
              }
            }
          }
        }
        const std::uint32_t fps = request->max_camera_fps() == 0 ? 30 : request->max_camera_fps();
        MonitorSample initial =
            core_.ReadMonitor(request->camera_device_id(), request->microphone_device_id());
        island::eos::v1::MonitorEvent ready_event;
        auto* ready = ready_event.mutable_ready();
        ready->set_camera_device_id(request->camera_device_id());
        ready->set_microphone_device_id(request->microphone_device_id());
        ready->set_camera_fps(fps);
        if (initial.camera) {
          ready->set_camera_width_px(initial.camera->width_px);
          ready->set_camera_height_px(initial.camera->height_px);
        }
        if (initial.audio) {
          ready->set_audio_sample_rate_hz(initial.audio->sample_rate_hz);
          ready->set_audio_channels(initial.audio->channels);
        }
        if (request->include_detections()) {
          if (request->camera_device_id() != detection_camera) {
            ready->set_detection_status("not_configured");
          } else if (object_detections) {
            ready->set_detection_status(
                core_.services_.camera_object != nullptr &&
                        ModelHealth(core_.services_.models,
                                    core_.services_.object_model_id) == "ready"
                    ? "available" : "unavailable");
          } else {
            bool available = core_.services_.camera_pose != nullptr;
            for (const auto& role : core_.services_.perception_roles) {
              if (role.role == "human")
                available = available &&
                    ModelHealth(core_.services_.models, role.model_id) == "ready";
            }
            ready->set_detection_status(available ? "available" : "unavailable");
          }
        } else {
          ready->set_detection_status("unavailable");
        }
        if (!writer->Write(ready_event)) return grpc::Status::OK;

        const auto interval = std::chrono::microseconds(1000000 / fps);
        auto next = std::chrono::steady_clock::now();
        std::uint64_t last_camera_sequence = 0;
        std::uint64_t last_audio_sequence = 0;
        std::uint64_t last_detection_sequence = 0;
        while (!context->IsCancelled()) {
          MonitorSample sample =
              core_.ReadMonitor(request->camera_device_id(), request->microphone_device_id());
          if (sample.camera) {
            if (sample.camera->sequence != last_camera_sequence) {
              island::eos::v1::MonitorEvent event;
              auto* camera = event.mutable_camera();
              camera->set_jpeg(sample.camera->jpeg.data(), sample.camera->jpeg.size());
              camera->set_width_px(sample.camera->width_px);
              camera->set_height_px(sample.camera->height_px);
              camera->set_sequence(sample.camera->sequence);
              camera->set_dropped_frames(last_camera_sequence == 0
                                             ? 0
                                             : sample.camera->sequence - last_camera_sequence - 1);
              if (sample.camera->source_timestamp_ns)
                camera->set_source_timestamp_ns(*sample.camera->source_timestamp_ns);
              camera->set_captured_at(CapturedAt(sample.camera->captured_at));
              if (!writer->Write(event)) break;
              last_camera_sequence = sample.camera->sequence;
            }
          }
          if (sample.audio && sample.audio->sequence != last_audio_sequence) {
            island::eos::v1::MonitorEvent event;
            auto* audio = event.mutable_audio();
            audio->set_rms(sample.audio->rms);
            audio->set_peak(sample.audio->peak);
            audio->set_dbfs(sample.audio->dbfs);
            audio->set_clipping(sample.audio->clipping);
            audio->set_sequence(sample.audio->sequence);
            if (sample.audio->source_timestamp_ns)
              audio->set_source_timestamp_ns(*sample.audio->source_timestamp_ns);
            audio->set_sample_rate_hz(sample.audio->sample_rate_hz);
            audio->set_channels(sample.audio->channels);
            audio->set_captured_at(CapturedAt(sample.audio->captured_at));
            if (!writer->Write(event)) break;
            last_audio_sequence = sample.audio->sequence;
          }
          if (request->include_detections() && object_detections &&
              request->camera_device_id() == detection_camera &&
              core_.services_.camera_object != nullptr) {
            const auto snapshot = core_.services_.camera_object->Latest(detection_camera);
            if (snapshot && snapshot->sequence != last_detection_sequence && snapshot->jpeg &&
                !snapshot->jpeg->empty()) {
              try {
                const auto* device = core_.services_.registry->FindDevice(snapshot->device_id);
                if (device == nullptr) throw std::runtime_error("detection camera unavailable");
                std::string robot_name;
                for (const auto& robot : core_.services_.registry->robots())
                  if (robot.id == device->robot_id()) robot_name = robot.name;
                if (robot_name.empty()) throw std::runtime_error("detection robot unavailable");
                const std::string uri = "robots/" + robot_name + "/cameras/" +
                                        detection_camera + "/detections/state";
                const nlohmann::json detection = core_.services_.projection->BuildByUri(uri);
                // A newer inference may have arrived between reading the store and projection.
                if (detection.value("source_sequence", 0ULL) != snapshot->sequence)
                  throw std::runtime_error("detection source changed during projection");
                island::eos::v1::MonitorEvent event;
                auto* frame = event.mutable_detection();
                frame->set_jpeg(snapshot->jpeg->data(), snapshot->jpeg->size());
                frame->set_width_px(snapshot->image_width_px);
                frame->set_height_px(snapshot->image_height_px);
                frame->set_sequence(snapshot->sequence);
                frame->set_generation(snapshot->generation);
                if (snapshot->source_timestamp_ns)
                  frame->set_source_timestamp_ns(*snapshot->source_timestamp_ns);
                frame->set_detection_json(detection.dump());
                if (!writer->Write(event)) break;
                last_detection_sequence = snapshot->sequence;
              } catch (const std::exception&) {
                // Keep raw camera frames available while detection is pending.
              }
            }
          }
          if (request->include_detections() && !object_detections &&
              request->camera_device_id() == core_.services_.perception_camera_device_id &&
              core_.services_.camera_pose != nullptr) {
            const auto snapshot = core_.services_.camera_pose->Latest();
            if (snapshot && snapshot->sequence != last_detection_sequence && snapshot->jpeg) {
              const auto* device = core_.services_.registry->FindDevice(snapshot->device_id);
              if (device != nullptr) {
                try {
                  std::string robot_name;
                  for (const auto& robot : core_.services_.registry->robots())
                    if (robot.id == device->robot_id()) robot_name = robot.name;
                  if (robot_name.empty()) throw std::runtime_error("detection robot unavailable");
                  const std::string uri = "robots/" + robot_name + "/detection/state";
                  const nlohmann::json detection = core_.services_.projection->BuildByUri(uri);
                  island::eos::v1::MonitorEvent event;
                  auto* frame = event.mutable_detection();
                  frame->set_jpeg(snapshot->jpeg->data(), snapshot->jpeg->size());
                  frame->set_width_px(snapshot->image_width_px);
                  frame->set_height_px(snapshot->image_height_px);
                  frame->set_sequence(snapshot->sequence);
                  frame->set_generation(snapshot->generation);
                  if (snapshot->source_timestamp_ns)
                    frame->set_source_timestamp_ns(*snapshot->source_timestamp_ns);
                  frame->set_detection_json(detection.dump());
                  if (!writer->Write(event)) break;
                  last_detection_sequence = snapshot->sequence;
                } catch (const std::exception&) {
                  // A raw monitor stream remains useful while perception is pending.
                }
              }
            }
          }
          next += interval;
          std::this_thread::sleep_until(next);
        }
        return grpc::Status::OK;
      });
}

}  // namespace eos::runtime::northbound
