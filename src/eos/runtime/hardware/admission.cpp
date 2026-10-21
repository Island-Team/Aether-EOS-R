#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "class.h"
#include "registry.h"
#include "src/eos/lib/log.h"
#include "src/eos/lib/uuid.h"

namespace eos::runtime::hardware_runtime {
namespace {
void CheckEnvelope(const std::string& path, const std::string& device_id, const char* feed_class,
                   const hardware::FeedEnvelope& declared, const hardware::FeedEnvelope& envelope) {
  if (!std::isfinite(declared.max_rate_hz) || declared.max_rate_hz < 0.0) {
    lib::LogEvent(lib::GetLogger("hardware"), spdlog::level::err, "device_admission_failed",
                  {{"device_id", device_id},
                   {"feed_class", feed_class},
                   {"reason", "invalid_feed_rate"},
                   {"declared_rate_hz", declared.max_rate_hz}});
    throw std::runtime_error("binding '" + path + "' device '" + device_id + "': feed '" +
                             feed_class + "' declares a non-finite or negative rate");
  }
  if (declared.max_rate_hz > envelope.max_rate_hz) {
    lib::LogEvent(lib::GetLogger("hardware"), spdlog::level::err, "device_admission_failed",
                  {{"device_id", device_id},
                   {"feed_class", feed_class},
                   {"reason", "feed_rate_exceeds_limit"},
                   {"declared_rate_hz", declared.max_rate_hz},
                   {"limit_rate_hz", envelope.max_rate_hz}});
    std::ostringstream message;
    message << "binding '" << path << "' device '" << device_id << "': feed '" << feed_class
            << "' exceeds rate limit: backend declares " << declared.max_rate_hz
            << " Hz; configuration allows " << envelope.max_rate_hz << " Hz";
    throw std::runtime_error(message.str());
  }
}

const char* KindName(hardware::DeviceKind kind) {
  switch (kind) {
    case hardware::DeviceKind::kManipulator:
      return "manipulator";
    case hardware::DeviceKind::kBase:
      return "base";
    case hardware::DeviceKind::kGimbal:
      return "gimbal";
    case hardware::DeviceKind::kCamera:
      return "camera";
    case hardware::DeviceKind::kMicrophone:
      return "microphone";
    case hardware::DeviceKind::kGripper:
      return "gripper";
    case hardware::DeviceKind::kSpeaker:
      return "speaker";
  }
  return "unknown";
}

hardware::DeviceKind KindOf(const std::string& part_kind) {
  if (part_kind == "base") return hardware::DeviceKind::kBase;
  if (part_kind == "gimbal") return hardware::DeviceKind::kGimbal;
  if (part_kind == "cam") return hardware::DeviceKind::kCamera;
  if (part_kind == "mic") return hardware::DeviceKind::kMicrophone;
  if (part_kind == "gripper") return hardware::DeviceKind::kGripper;
  if (part_kind == "speaker") return hardware::DeviceKind::kSpeaker;
  return hardware::DeviceKind::kManipulator;
}

}  // namespace

Registry Registry::Load(const BackendCatalog& catalog, const std::string& binding_path,
                        const std::string& profile, const std::string& runtime_id) {
  auto logger = lib::GetLogger("hardware");
  lib::LogEvent(logger, spdlog::level::info, "binding_load_started", {{"profile", profile}});
  try {
    return Adopt(catalog, LoadBinding(binding_path, profile, catalog), runtime_id);
  } catch (...) {
    lib::LogEvent(logger, spdlog::level::err, "binding_load_failed",
                  {{"profile", profile}, {"reason", "load_or_admission_error"}});
    throw;
  }
}

Registry Registry::Load(const BackendCatalog& catalog,
                        const std::vector<std::pair<std::string, std::string>>& bindings,
                        const std::string& runtime_id) {
  std::vector<Binding> parsed;
  parsed.reserve(bindings.size());
  auto logger = lib::GetLogger("hardware");
  lib::LogEvent(logger, spdlog::level::info, "binding_composition_started",
                {{"bindings", bindings.size()}});
  try {
    for (const auto& [path, profile] : bindings)
      parsed.push_back(LoadBinding(path, profile, catalog));
    return Adopt(catalog, std::move(parsed), runtime_id);
  } catch (...) {
    lib::LogEvent(logger, spdlog::level::err, "binding_composition_failed",
                  {{"bindings", bindings.size()}, {"reason", "load_or_admission_error"}});
    throw;
  }
}

Registry Registry::Adopt(const BackendCatalog& catalog, Binding binding,
                         const std::string& runtime_id) {
  std::vector<Binding> bindings;
  bindings.push_back(std::move(binding));
  return Adopt(catalog, std::move(bindings), runtime_id);
}

Registry Registry::Adopt(const BackendCatalog& catalog, std::vector<Binding> bindings,
                         const std::string& runtime_id) {
  Registry registry;
  if (bindings.size() != 1) {
    lib::LogEvent(lib::GetLogger("hardware"), spdlog::level::err, "binding_admission_failed",
                  {{"bindings", bindings.size()}, {"reason", "binding_count"}});
    throw std::runtime_error("registry: exactly one hardware binding is required per EOS process");
  }
  const std::string fallback_runtime_id = runtime_id.empty() ? lib::MintUuidV7() : runtime_id;
  std::unordered_map<std::string, std::size_t> class_indexes;
  for (Binding& binding : bindings) {
    if (binding.devices.empty()) {
      lib::LogEvent(lib::GetLogger("hardware"), spdlog::level::err, "binding_admission_failed",
                    {{"profile", binding.profile}, {"reason", "no_devices"}});
      throw std::runtime_error("binding '" + binding.path + "': no devices to admit");
    }
    // An explicitly injected test/operator ID wins. Normal runtime startup
    // passes no override so the binding's stable robot_id is authoritative.
    if (!runtime_id.empty()) {
      binding.robot.id = runtime_id;
    } else if (binding.robot.id.empty()) {
      binding.robot.id = fallback_runtime_id;
      lib::LogEvent(lib::GetLogger("hardware"), spdlog::level::warn, "robot_id_generated",
                    {{"profile", binding.profile},
                     {"robot_id", binding.robot.id},
                     {"reason", "robot_id_not_configured"}});
    }
    registry.robots_.push_back(binding.robot);
    for (DeviceBinding& spec : binding.devices) {
      spec.device_id = spec.part.kind + "-" + std::to_string(class_indexes[spec.part.kind]++);
      const BackendDescriptor* descriptor = catalog.Find(spec.backend_type);
      if (descriptor == nullptr || !descriptor->supports_class ||
          !descriptor->supports_class(spec.part.kind) || !descriptor->compiled ||
          !descriptor->create) {
        const bool guarded = descriptor != nullptr && !descriptor->compiled;
        lib::LogEvent(lib::GetLogger("hardware"), spdlog::level::err, "device_admission_failed",
                      {{"device_id", spec.device_id},
                       {"backend", spec.backend_type},
                       {"class", spec.part.kind},
                       {"reason", guarded ? "backend_not_compiled" : "unsupported_backend"}});
        throw std::runtime_error("binding '" + binding.path + "': backend type '" +
                                 spec.backend_type +
                                 (guarded ? "' is not compiled into this build"
                                          : "' does not implement class '" + spec.part.kind + "'") +
                                 " (device '" + spec.device_id + "')");
      }
      if (!spec.backend_settings)
        throw std::runtime_error("binding '" + binding.path + "': backend settings missing");
      std::unique_ptr<hardware::Backend> backend =
          descriptor->create(spec.device_id, spec.part, *spec.backend_settings);
      if (backend == nullptr)
        throw std::runtime_error("binding '" + binding.path + "': backend factory returned null");
      const hardware::BackendIdentity identity = backend->Identify();
      const hardware::DeviceKind expected_kind = KindOf(spec.part.kind);
      if (identity.name.empty() || identity.version.empty() ||
          identity.device_id != spec.device_id || identity.kind != expected_kind) {
        lib::LogEvent(lib::GetLogger("hardware"), spdlog::level::err, "device_admission_failed",
                      {{"device_id", spec.device_id},
                       {"backend", spec.backend_type},
                       {"class", spec.part.kind},
                       {"reason", "invalid_identity"},
                       {"reported_device_id", identity.device_id},
                       {"reported_kind", KindName(identity.kind)}});
        throw std::runtime_error("binding '" + binding.path + "' device '" + spec.device_id +
                                 "': backend reported an inconsistent identity");
      }
      const hardware::FeedDeclarations declared = backend->DeclareFeeds();
      CheckEnvelope(binding.path, spec.device_id, "geometric", declared.geometric,
                    spec.envelope.geometric);
      CheckEnvelope(binding.path, spec.device_id, "proprioception", declared.proprioception,
                    spec.envelope.proprioception);
      CheckEnvelope(binding.path, spec.device_id, "sensor", declared.sensor, spec.envelope.sensor);
      CheckEnvelope(binding.path, spec.device_id, "health", declared.health, spec.envelope.health);
      backend->MarkAdmitted();

      registry.devices_.emplace_back(std::move(backend), binding.robot.id, std::move(spec.part),
                                     spec.envelope);
      const Device& device = registry.devices_.back();
      lib::LogEvent(lib::GetLogger("hardware"), spdlog::level::info, "device_admitted",
                    {{"device_id", device.identity().device_id},
                     {"robot_id", device.robot_id()},
                     {"backend", spec.backend_type},
                     {"class", device.part().kind},
                     {"kind", KindName(device.identity().kind)},
                     {"primitives", device.part().primitives.size()}});
    }
  }
  registry.robot_ = registry.robots_.front();
  lib::LogEvent(lib::GetLogger("hardware"), spdlog::level::info, "binding_admitted",
                {{"profile", bindings.front().profile},
                 {"robot_id", registry.robot_.id},
                 {"devices", registry.devices_.size()}});
  return registry;
}
}  // namespace eos::runtime::hardware_runtime
