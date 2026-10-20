#include "dispatcher.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

#include "common.h"
#include "src/eos/lib/log.h"
#include "src/eos/runtime/init/schemas.h"

namespace eos::runtime::dispatch {
using namespace detail;
namespace {

constexpr std::size_t kMaximumDiagnosticDetailBytes = 512;

std::string DiagnosticDetail(std::string detail) {
  if (detail.size() <= kMaximumDiagnosticDetailBytes) return detail;
  detail.resize(kMaximumDiagnosticDetailBytes);
  detail += "...";
  return detail;
}

nlohmann::json DispatchFields(const std::string& primitive, const std::string& version,
                              const std::string& device_id, const nlohmann::json& arguments) {
  nlohmann::json fields{{"primitive", primitive}, {"primitive_version", version}};
  if (!device_id.empty()) fields["device_id"] = device_id;
  const auto command_id = arguments.find("command_id");
  if (command_id != arguments.end() && command_id->is_string()) {
    fields["command_id"] = command_id->get<std::string>();
  }
  return fields;
}

nlohmann::json EffectiveParameters(
    const init::SchemaStore& schemas, const std::string& name, const std::string& version,
    const hardware_runtime::PrimitiveDeclaration* declaration = nullptr,
    const hardware_runtime::Device* device = nullptr) {
  nlohmann::json schema = schemas.PrimitiveSchema(name, version);
  auto& properties = schema["properties"];
  properties.erase("type");
  properties.erase("command_id");

  if (schema.contains("required") && schema.at("required").is_array()) {
    auto& required = schema["required"];
    required.erase(std::remove(required.begin(), required.end(), "type"), required.end());
    required.erase(std::remove(required.begin(), required.end(), "command_id"), required.end());
    if (required.empty()) schema.erase("required");
  }

  if (declaration != nullptr) {
    if (properties.contains("reference"))
      properties["reference"]["enum"] = declaration->references;
    if (properties.contains("frame_id")) {
      if (declaration->absolute_frame)
        properties["frame_id"]["enum"] = {*declaration->absolute_frame};
      else
        properties.erase("frame_id");
    }

    std::set<std::string> required;
    if (schema.contains("required")) {
      for (const auto& item : schema.at("required"))
        if (item.is_string()) required.insert(item.get<std::string>());
    }
    std::vector<std::string> unsupported;
    for (const auto& [argument, property] : properties.items()) {
      if (required.count(argument) == 0 && property.is_object() &&
          (property.value("type", "") == "number" ||
           property.value("type", "") == "integer") &&
          declaration->limits.count(argument) == 0)
        unsupported.push_back(argument);
    }
    for (const auto& argument : unsupported) properties.erase(argument);
  }

  if (device != nullptr && name == "arm.movej" && properties.contains("joint_positions_rad")) {
    const std::size_t joints =
        device->part().joint_names.empty() ? 6 : device->part().joint_names.size();
    properties["joint_positions_rad"]["minItems"] = joints;
    properties["joint_positions_rad"]["maxItems"] = joints;
  }
  return schema;
}

void LogRejected(const std::shared_ptr<spdlog::logger>& logger, const std::string& primitive,
                 const std::string& version, const std::string& device_id,
                 const nlohmann::json& arguments, const char* reason,
                 std::chrono::steady_clock::time_point started, const std::string& detail = {}) {
  nlohmann::json fields = DispatchFields(primitive, version, device_id, arguments);
  fields["accepted"] = false;
  fields["reason"] = reason;
  if (!detail.empty()) fields["detail"] = DiagnosticDetail(detail);
  fields["duration_ms"] =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  lib::LogEvent(logger, spdlog::level::warn, "primitive_rejected", std::move(fields));
}

}  // namespace

Dispatcher::Dispatcher(hardware_runtime::Registry& registry, const init::SchemaStore& schemas,
                       bool expose_operator, models::Runtime* models,
                       std::filesystem::path assets_root, std::filesystem::path command_journal,
                       AdmissionOptions admission)
    : registry_(registry),
      schemas_(schemas),
      expose_operator_(expose_operator),
      models_(models),
      assets_root_(std::move(assets_root)),
      command_journal_(std::move(command_journal)),
      admission_(admission) {
  if (admission_.max_commands_per_device == 0 || admission_.max_wait.count() <= 0)
    throw std::invalid_argument("dispatch admission limits must be greater than zero");
  for (const hardware_runtime::Device& device : registry.devices()) {
    device_mutexes_.emplace(device.identity().device_id, std::make_unique<std::mutex>());
  }
  LoadCommandJournal();
}

Dispatcher::~Dispatcher() {
  std::size_t clamped = 0;
  std::size_t failed = 0;
  for (hardware_runtime::Device& device : registry_.devices_) {
    try {
      device.backend().Clamp();
      ++clamped;
    } catch (const std::exception& error) {
      ++failed;
      lib::LogEvent(lib::GetLogger("dispatch"), spdlog::level::err, "shutdown_clamp_failed",
                    {{"device_id", device.identity().device_id}, {"detail", error.what()}});
    } catch (...) {
      ++failed;
      lib::LogEvent(
          lib::GetLogger("dispatch"), spdlog::level::err, "shutdown_clamp_failed",
          {{"device_id", device.identity().device_id}, {"detail", "unknown backend exception"}});
    }
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    base_velocity_lease_s_.clear();
  }
  if (clamped != 0 || failed != 0) {
    lib::LogEvent(lib::GetLogger("dispatch"), spdlog::level::info, "shutdown_clamp_completed",
                  {{"clamped", clamped}, {"failed", failed}});
  }
}

std::mutex* Dispatcher::DeviceMutex(const std::string& device_id) const {
  const auto found = device_mutexes_.find(device_id);
  return found == device_mutexes_.end() ? nullptr : found->second.get();
}

bool Dispatcher::DeclaresPrimitive(const hardware_runtime::Device& device,
                                   const std::string& primitive, const std::string& version) {
  return std::any_of(device.part().primitives.begin(), device.part().primitives.end(),
                     [&](const hardware_runtime::PrimitiveDeclaration& item) {
                       return item.name == primitive && item.version == version;
                     });
}

hardware_runtime::Device* Dispatcher::Target(
    const std::string& primitive, const std::string& version, const std::string& device_id,
    hardware::DeviceKind kind, const nlohmann::json& arguments, nlohmann::json* rejection) {
  if (device_id.empty()) {
    *rejection = Ack(primitive, version, arguments, false, kInvalidTarget,
                     "device_id is required for device primitives");
    return nullptr;
  }
  hardware_runtime::Device* device = registry_.FindDeviceForRelease(device_id);
  if (device == nullptr) {
    *rejection = Ack(primitive, version, arguments, false, kInvalidTarget,
                     "unknown device_id '" + device_id + "'");
    return nullptr;
  }
  if (device->backend().Identify().kind != kind) {
    *rejection = Ack(primitive, version, arguments, false, kInvalidTarget,
                     "device '" + device_id + "' has the wrong class");
    return nullptr;
  }
  if (!DeclaresPrimitive(*device, primitive, version)) {
    *rejection = Ack(primitive, version, arguments, false, kUnsupported,
                     "device '" + device_id + "' does not declare primitive '" + primitive +
                         "' version '" + version + "'");
    return nullptr;
  }
  return device;
}

DispatchResult Dispatcher::Execute(const std::string& primitive, const std::string& version,
                                   const std::string& device_id, nlohmann::json arguments) {
  static auto logger = lib::GetLogger("dispatch");
  const auto started = std::chrono::steady_clock::now();
  lib::LogEvent(logger, spdlog::level::debug, "primitive_started",
                DispatchFields(primitive, version, device_id, arguments));
  if (primitive.empty()) {
    LogRejected(logger, primitive, version, device_id, arguments, "missing_primitive", started);
    return {DispatchResult::Status::kInvalidArgument, nullptr, "name is required"};
  }
  if (version.empty()) {
    LogRejected(logger, primitive, version, device_id, arguments, "missing_version", started);
    return {DispatchResult::Status::kInvalidArgument, nullptr, "version is required"};
  }
  if (!arguments.is_object()) {
    LogRejected(logger, primitive, version, device_id, arguments, "arguments_not_object", started);
    return {DispatchResult::Status::kInvalidArgument, nullptr, "arguments must be an object"};
  }
  if (!schemas_.HasPrimitive(primitive, version)) {
    LogRejected(logger, primitive, version, device_id, arguments, "unknown_primitive", started);
    return {DispatchResult::Status::kNotFound, nullptr,
            "unknown primitive '" + primitive + "' version '" + version + "'"};
  }
  if (!arguments.contains("type")) arguments["type"] = primitive;
  try {
    schemas_.ValidatePrimitive(primitive, version, arguments);
  } catch (const init::ValidationError& error) {
    LogRejected(logger, primitive, version, device_id, arguments, "schema_validation", started,
                error.what());
    return {DispatchResult::Status::kInvalidArgument, nullptr, error.what()};
  }
  // Backend calls serialize per device, while independent devices may execute
  // concurrently. Speaker preparation is intentionally outside this lock: model
  // and asset I/O cannot delay a safety halt, and SpeakerPlay/SpeakerSpeak acquire
  // the device lock immediately before releasing audio to the backend.
  std::unique_lock<std::mutex> device_lock;
  hardware_runtime::Device* stop_device = primitive == "motion.stop"
      ? registry_.FindDeviceForRelease(device_id) : nullptr;
  const bool concurrent_stop = stop_device != nullptr &&
      stop_device->backend().CanStopWithoutDeviceLock();
  if (primitive != "safety.estop" && primitive != "robot.stop" && primitive != "speaker.play" &&
      primitive != "speaker.speak" && !concurrent_stop) {
    if (std::mutex* device_mutex = DeviceMutex(device_id)) {
      device_lock = std::unique_lock<std::mutex>(*device_mutex);
    }
  }
  try {
    nlohmann::json payload = ExecuteAdmitted(primitive, version, device_id, arguments);
    const auto accepted = payload.find("accepted");
    nlohmann::json fields = DispatchFields(primitive, version, device_id, arguments);
    fields["duration_ms"] =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    if (accepted != payload.end() && accepted->is_boolean() && !accepted->get<bool>()) {
      const auto reason = payload.find("reason_code");
      fields["reason"] =
          reason != payload.end() && reason->is_string() ? reason->get<std::string>() : "admission";
      fields["accepted"] = false;
      const auto detail = payload.find("reason");
      if (detail != payload.end() && detail->is_string()) {
        fields["detail"] = DiagnosticDetail(detail->get<std::string>());
      }
      lib::LogEvent(logger, spdlog::level::warn, "primitive_rejected", std::move(fields));
    } else {
      fields["accepted"] = true;
      lib::LogEvent(logger, spdlog::level::debug, "primitive_completed", std::move(fields));
    }
    return {DispatchResult::Status::kCompleted, std::move(payload), {}};
  } catch (const std::exception& error) {
    nlohmann::json fields = DispatchFields(primitive, version, device_id, arguments);
    fields["reason"] = "backend_exception";
    fields["detail"] = DiagnosticDetail(error.what());
    fields["duration_ms"] =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    lib::LogEvent(logger, spdlog::level::err, "primitive_failed", std::move(fields));
    return {DispatchResult::Status::kCompleted,
            Ack(primitive, version, arguments, false, kInternal, error.what()),
            {}};
  } catch (...) {
    nlohmann::json fields = DispatchFields(primitive, version, device_id, arguments);
    fields["reason"] = "unknown_backend_exception";
    fields["duration_ms"] =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    lib::LogEvent(logger, spdlog::level::err, "primitive_failed", std::move(fields));
    return {DispatchResult::Status::kCompleted,
            Ack(primitive, version, arguments, false, kInternal, "backend release failed"),
            {}};
  }
}

DispatchResult Dispatcher::ExecuteSkillStep(const std::string& primitive,
                                            const std::string& version,
                                            const std::string& device_id, nlohmann::json arguments,
                                            const std::function<bool()>& cancelled) {
  const std::string command_id = arguments.value("command_id", "");
  return ExecuteCommand(primitive, version, device_id, std::move(arguments), command_id, cancelled);
}

DispatchResult Dispatcher::ExecuteTerminal(const std::string& primitive, const std::string& version,
                                            const std::string& device_id, nlohmann::json arguments,
                                            const std::function<bool()>& cancelled,
                                            const std::function<void()>& on_backend_started) {
  if (cancelled && cancelled()) {
    DispatchResult result{DispatchResult::Status::kCompleted,
                          Ack(primitive, version, arguments, false, kBusy,
                              "command was cancelled before backend release"),
                          {}};
    result.payload["result"] = {{"status", "cancelled"}};
    return result;
  }

  std::optional<hardware::GimbalTelemetry> initial_gimbal;
  if (primitive == "gimbal.motion") {
    if (const hardware_runtime::Device* device = registry_.FindDevice(device_id)) {
      initial_gimbal = device->feeds().ReadGimbalTelemetry();
    }
  }

  const nlohmann::json step_arguments = arguments;
  if (primitive == "gimbal.motion" && !arguments.value("stream", false) && initial_gimbal &&
      arguments.value("reference", "") == "relative" && arguments.contains("pan_rad") &&
      arguments.contains("tilt_rad")) {
    arguments["reference"] = "absolute";
    arguments["pan_rad"] = arguments.at("pan_rad").get<double>() + initial_gimbal->pan_rad;
    arguments["tilt_rad"] = arguments.at("tilt_rad").get<double>() + initial_gimbal->tilt_rad;
  }
  const nlohmann::json released_arguments = arguments;
  if (on_backend_started) on_backend_started();
  DispatchResult released = Execute(primitive, version, device_id, std::move(arguments));
  if (released.status != DispatchResult::Status::kCompleted || !released.payload.is_object() ||
      !released.payload.value("accepted", false)) {
    return released;
  }
  if (released.payload.contains("result") && released.payload.at("result").is_object() &&
      released.payload.at("result").value("status", "succeeded") != "succeeded") {
    return released;
  }
  if (step_arguments.value("stream", false)) return released;
  if (primitive == "robot.stop") return released;

  auto terminal = [&](const char* status, const std::string& reason = {}) {
    released.payload["result"] = {{"status", status}};
    if (!reason.empty()) released.payload["result"]["reason"] = reason;
    return released;
  };
  auto stop = [&] {
    nlohmann::json stop_arguments{{"type", "motion.stop"}};
    const std::string command_id = step_arguments.value("command_id", "");
    if (!command_id.empty()) stop_arguments["command_id"] = command_id + "/stop";
    Execute("motion.stop", "1.0.0", device_id, std::move(stop_arguments));
  };

  // A velocity command remains active for its complete lease. Keeping the
  // command record pending makes that interval cancellable and prevents a
  // duplicate command ID from releasing the backend again.
  if (primitive == "base.velocity") {
    const auto lease = step_arguments.find("valid_for_ms");
    if (lease == step_arguments.end() || !lease->is_number_integer()) {
      return terminal("failed", "velocity lease is unavailable");
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(lease->get<std::int64_t>());
    constexpr auto kVelocityPollInterval = std::chrono::milliseconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      if (cancelled && cancelled()) {
        stop();
        return terminal("cancelled", "velocity command was cancelled");
      }
      std::this_thread::sleep_for(kVelocityPollInterval);
    }
    stop();
    return terminal("succeeded");
  }

  if (primitive == "speaker.play" || primitive == "speaker.speak") {
    constexpr auto kSpeakerMinimumTimeout = std::chrono::seconds(15);
    constexpr auto kSpeakerMaximumTimeout = std::chrono::minutes(5);
    constexpr auto kSpeakerCompletionMargin = std::chrono::seconds(5);
    constexpr auto kSpeakerPollInterval = std::chrono::milliseconds(20);
    const hardware_runtime::Device* initial_device = registry_.FindDevice(device_id);
    const auto initial =
        initial_device ? initial_device->feeds().ReadSpeakerTelemetry() : std::nullopt;
    auto timeout = kSpeakerMinimumTimeout;
    if (initial && initial->queued_duration_ms) {
      timeout = std::max(timeout, std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::milliseconds(*initial->queued_duration_ms) +
                                      kSpeakerCompletionMargin));
    }
    timeout =
        std::min(timeout, std::chrono::duration_cast<std::chrono::seconds>(kSpeakerMaximumTimeout));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (cancelled && cancelled()) {
        if (hardware_runtime::Device* device = registry_.FindDeviceForRelease(device_id)) {
          std::lock_guard<std::mutex> lock(*DeviceMutex(device_id));
          device->backend().Clamp();
        }
        return terminal("cancelled", "speaker command was cancelled");
      }
      const hardware_runtime::Device* device = registry_.FindDevice(device_id);
      const auto observed = device ? device->feeds().ReadSpeakerTelemetry() : std::nullopt;
      if (!observed) return terminal("failed", "speaker playback state is unavailable");
      if (observed->state == hardware::SpeakerPlaybackState::kIdle) {
        return terminal("succeeded");
      }
      if (observed->state == hardware::SpeakerPlaybackState::kFault) {
        // Prefer the backend's own cause; fall back to the generic text when the
        // backend keeps the base default.
        std::string reason = device ? device->backend().ReleaseFailureReason() : std::string{};
        if (reason.empty() || reason == "backend refused the release") {
          reason = "speaker reported a playback fault";
        }
        return terminal("failed", reason);
      }
      std::this_thread::sleep_for(kSpeakerPollInterval);
    }
    if (hardware_runtime::Device* device = registry_.FindDeviceForRelease(device_id)) {
      std::lock_guard<std::mutex> lock(*DeviceMutex(device_id));
      device->backend().Clamp();
    }
    return terminal("timed_out", "speaker did not finish before its terminal deadline");
  }

  // Every other backend operation returns at its documented terminal boundary.
  if (primitive != "gimbal.motion") return terminal("succeeded");

  constexpr auto kCompletionTimeout = std::chrono::seconds(30);
  constexpr auto kPollInterval = std::chrono::milliseconds(20);
  constexpr auto kReadinessPropagation = std::chrono::milliseconds(250);
  constexpr double kPositionToleranceRad = 0.02;
  constexpr double kRateToleranceRadS = 0.05;
  if (!initial_gimbal || !step_arguments.contains("reference") ||
      !step_arguments.contains("pan_rad") || !step_arguments.contains("tilt_rad")) {
    return terminal("failed", "gimbal completion target is unavailable");
  }

  const double target_pan = released_arguments.at("pan_rad").get<double>();
  const double target_tilt = released_arguments.at("tilt_rad").get<double>();
  const auto not_before = std::chrono::steady_clock::now() + kReadinessPropagation;
  const auto deadline = std::chrono::steady_clock::now() + kCompletionTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cancelled && cancelled()) {
      return terminal("cancelled", "skill execution was cancelled");
    }
    const hardware_runtime::Device* device = registry_.FindDevice(device_id);
    const auto observed = device ? device->feeds().ReadGimbalTelemetry() : std::nullopt;
    if (std::chrono::steady_clock::now() >= not_before && device && device->ReadHealth().alive &&
        observed && std::abs(observed->pan_rad - target_pan) <= kPositionToleranceRad &&
        std::abs(observed->tilt_rad - target_tilt) <= kPositionToleranceRad &&
        std::abs(observed->pan_rate_rad_s) <= kRateToleranceRadS &&
        std::abs(observed->tilt_rate_rad_s) <= kRateToleranceRadS) {
      return terminal("succeeded");
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  stop();
  return terminal("timed_out", "gimbal did not reach its target within 30 seconds");
}

nlohmann::json Dispatcher::ExecuteAdmitted(const std::string& primitive, const std::string& version,
                                           const std::string& device_id,
                                           const nlohmann::json& arguments) {
  const hardware_runtime::PrimitiveSpec* spec =
      hardware_runtime::PrimitiveSpecOf(primitive, version);
  if (spec == nullptr)
    return Ack(primitive, version, arguments, false, kUnsupported,
               "unknown primitive '" + primitive + "' version '" + version + "'");
  if (registry_.EstopLatched() && primitive != "safety.estop" && primitive != "robot.stop" &&
      primitive != "motion.stop")
    return Ack(primitive, version, arguments, false, kEstop,
               "estop latched; only safety.estop, robot.stop, and motion.stop are admitted");
  if (primitive == "safety.estop") {
    if (!device_id.empty())
      return Ack(primitive, version, arguments, false, kInvalidArgument,
                 "device_id must be empty for process-wide safety.estop");
    return Estop(primitive, version, arguments);
  }
  if (primitive == "motion.stop") {
    if (device_id.empty())
      return Ack(primitive, version, arguments, false, kInvalidTarget,
                 "device_id is required for motion.stop");
    hardware_runtime::Device* device = registry_.FindDeviceForRelease(device_id);
    if (device == nullptr)
      return Ack(primitive, version, arguments, false, kInvalidTarget,
                 "unknown device_id '" + device_id + "'");
    if (!IsMotion(device->backend().Identify().kind))
      return Ack(primitive, version, arguments, false, kInvalidTarget,
                 "device '" + device_id + "' is not a motion device");
    return MotionStop(primitive, version, *device, arguments);
  }
  if (primitive == "robot.stop") {
    if (!device_id.empty())
      return Ack(primitive, version, arguments, false, kInvalidArgument,
                 "device_id must be empty for robot.stop");
    return RobotStop(primitive, version, arguments);
  }
  nlohmann::json rejection;
  hardware_runtime::Device* device =
      Target(primitive, version, device_id, KindOf(spec->device_class), arguments, &rejection);
  if (device == nullptr) return rejection;
  if (primitive == "base.velocity") return BaseVelocity(primitive, version, *device, arguments);
  if (primitive == "base.navigate") return BaseNavigate(primitive, version, *device, arguments);
  if (primitive == "base.posture") return BasePosture(primitive, version, *device, arguments);
  if (primitive == "gimbal.motion") return GimbalMotion(primitive, version, *device, arguments);
  if (primitive == "arm.init") return ArmInit(primitive, version, *device, arguments);
  if (primitive == "arm.movej") return ArmMoveJ(primitive, version, *device, arguments);
  if (primitive == "arm.movel") return ArmMoveL(primitive, version, *device, arguments);
  if (primitive == "arm.pick") return ArmPick(primitive, version, *device, arguments);
  if (primitive == "gripper.motion") return GripperMotion(primitive, version, *device, arguments);
  if (primitive == "speaker.speak") return SpeakerSpeak(primitive, version, *device, arguments);
  if (primitive == "face.register") return FaceRegister(primitive, version, *device, arguments);
  return SpeakerPlay(primitive, version, *device, arguments);
}

std::vector<init::PrimitiveIdentity> Dispatcher::PrimitiveIdentities() const {
  std::vector<init::PrimitiveIdentity> identities;
  for (const hardware_runtime::Device& device : registry_.devices()) {
    for (const hardware_runtime::PrimitiveDeclaration& primitive : device.part().primitives) {
      if (primitive.tier == hardware_runtime::PrimitiveTier::kOperator && !expose_operator_)
        continue;
      if (hardware_runtime::PrimitiveSpecOf(primitive.name, primitive.version) == nullptr ||
          !schemas_.HasPrimitive(primitive.name, primitive.version)) {
        continue;
      }
      identities.push_back({primitive.name, primitive.version});
    }
  }
  std::sort(identities.begin(), identities.end());
  identities.erase(std::unique(identities.begin(), identities.end()), identities.end());
  return identities;
}

void Dispatcher::TickHardware(double dt_s) {
  static auto logger = lib::GetLogger("hardware");
  for (hardware_runtime::Device& device : registry_.devices_) {
    const std::string device_id = device.identity().device_id;
    std::unique_lock<std::mutex> lock(*DeviceMutex(device_id));
    try {
      device.backend().Tick(dt_s);
    } catch (const std::exception& error) {
      lib::LogEvent(logger, spdlog::level::err, "backend_tick_failed",
                    {{"device_id", device_id},
                     {"operation", "tick"},
                     {"detail", DiagnosticDetail(error.what())}});
      continue;
    } catch (...) {
      lib::LogEvent(logger, spdlog::level::err, "backend_tick_failed",
                    {{"device_id", device_id},
                     {"operation", "tick"},
                     {"detail", "unknown backend exception"}});
      continue;
    }
  }
}

nlohmann::json Dispatcher::Discover() const {
  nlohmann::json listing = nlohmann::json::array();
  for (const char* name : {"robot.stop", "safety.estop"}) {
    const hardware_runtime::PrimitiveSpec* spec = hardware_runtime::PrimitiveSpecOf(name, "1.0.0");
    if (spec != nullptr && schemas_.HasPrimitive(name, "1.0.0")) {
      listing.push_back({{"name", spec->name},
                         {"deviceId", ""},
                         {"robotId", registry_.robot().id},
                         {"version", spec->version},
                         {"description", spec->description},
                         {"parameters", EffectiveParameters(schemas_, spec->name, spec->version)},
                         {"limits", nlohmann::json::object()},
                         {"kind", "direct"},
                         {"references", nlohmann::json::array()},
                         {"absoluteFrame", nullptr},
                         {"tier", "PRIMITIVE_TIER_PRIVILEGED"}});
    }
  }
  for (const hardware_runtime::Device& device : registry_.devices()) {
    if (IsMotion(device.identity().kind) && schemas_.HasPrimitive("motion.stop", "1.0.0")) {
      const hardware_runtime::PrimitiveSpec* spec =
          hardware_runtime::PrimitiveSpecOf("motion.stop", "1.0.0");
      listing.push_back({{"name", spec->name},
                         {"deviceId", device.identity().device_id},
                         {"robotId", device.robot_id()},
                         {"version", spec->version},
                         {"description", spec->description},
                         {"deviceDescription", spec->description},
                         {"parameters", EffectiveParameters(schemas_, spec->name, spec->version)},
                         {"limits", nlohmann::json::object()},
                         {"kind", "direct"},
                         {"references", nlohmann::json::array()},
                         {"absoluteFrame", nullptr},
                         {"tier", "PRIMITIVE_TIER_PRIVILEGED"}});
    }
    for (const hardware_runtime::PrimitiveDeclaration& primitive : device.part().primitives) {
      if (primitive.tier == hardware_runtime::PrimitiveTier::kOperator && !expose_operator_)
        continue;
      const hardware_runtime::PrimitiveSpec* spec =
          hardware_runtime::PrimitiveSpecOf(primitive.name, primitive.version);
      if (spec == nullptr || !schemas_.HasPrimitive(primitive.name, primitive.version)) continue;
      nlohmann::json limits = nlohmann::json::object();
      for (const auto& [argument, range] : primitive.limits) {
        nlohmann::json interval = {{"minimum_exclusive", range.minimum_exclusive},
                                   {"maximum_exclusive", range.maximum_exclusive}};
        if (range.minimum.has_value()) interval["minimum"] = *range.minimum;
        if (range.maximum.has_value()) interval["maximum"] = *range.maximum;
        limits[argument] = std::move(interval);
      }
      listing.push_back(
          {{"name", spec->name},
           {"deviceId", device.identity().device_id},
           {"robotId", device.robot_id()},
           {"version", primitive.version},
           {"description", spec->description},
           {"deviceDescription", primitive.description},
           {"parameters", EffectiveParameters(schemas_, primitive.name, primitive.version,
                                               &primitive, &device)},
           {"limits", std::move(limits)},
           {"kind",
            spec->kind == hardware_runtime::PrimitiveKind::kAdvanced ? "advanced" : "direct"},
           {"references", primitive.references},
           {"absoluteFrame", primitive.absolute_frame.has_value()
                                 ? nlohmann::json(*primitive.absolute_frame)
                                 : nlohmann::json()},
           {"tier", primitive.tier == hardware_runtime::PrimitiveTier::kOperator
                        ? "PRIMITIVE_TIER_OPERATOR"
                        : "PRIMITIVE_TIER_STANDARD"}});
    }
  }
  return {{"primitives", std::move(listing)}};
}

}  // namespace eos::runtime::dispatch
