#include <chrono>
#include <utility>

#include "detail.h"
#include "reader.h"
#include "src/eos/lib/log.h"

namespace eos::runtime::perception::body {
using namespace detail;
namespace {

constexpr std::size_t kMaximumDiagnosticDetailBytes = 512;

std::string DiagnosticDetail(const std::string& detail) {
  if (detail.size() <= kMaximumDiagnosticDetailBytes) return detail;
  return detail.substr(0, kMaximumDiagnosticDetailBytes) + "...";
}

}  // namespace

bool Reader::SupportsResourceTemplate(const std::string& uri_template) const {
  return uri_template == "robots/{robot_name}/detection/state" ||
         uri_template == "robots/{robot_name}/detection/head/state" ||
         uri_template == "robots/{robot_name}/cameras/{camera_device_id}/detections/state" ||
         uri_template == "robots/{robot_name}/devices/{device_id}/state" ||
         uri_template == "robots/{robot_name}/world/vad/state" ||
         uri_template == "robots/{robot_name}/world/asr/state" ||
         uri_template == "robots/{robot_name}/arm/state" ||
         uri_template == "robots/{robot_name}/base/status" ||
         uri_template == "robots/{robot_name}/arm/status" ||
         uri_template == "robots/{robot_name}/gripper/status";
}

nlohmann::json Reader::BuildByUri(const std::string& uri, bool log_success) {
  std::lock_guard<std::mutex> resource_lock(resource_mutex_);
  static auto logger = lib::GetLogger("body");
  auto build = [&](const char* resource, const std::string& robot_name, const std::string& robot_id,
                   auto&& builder) -> nlohmann::json {
    const auto started = std::chrono::steady_clock::now();
    try {
      nlohmann::json result = builder();
      nlohmann::json fields{{"resource", resource},
                            {"uri", DiagnosticDetail(uri)},
                            {"robot_name", robot_name},
                            {"robot_id", robot_id},
                            {"bytes", result.dump().size()},
                            {"duration_ms", std::chrono::duration<double, std::milli>(
                                                std::chrono::steady_clock::now() - started)
                                                .count()}};
      const auto sequence = result.find("sequence");
      if (sequence != result.end() && sequence->is_number_unsigned()) {
        fields["sequence"] = sequence->get<std::uint64_t>();
      }
      if (log_success) {
        lib::LogEvent(logger, spdlog::level::debug, "resource_read_completed", std::move(fields));
      }
      return result;
    } catch (const ResourceUnavailable& error) {
      if (log_success) {
        lib::LogEvent(logger, spdlog::level::debug, "resource_read_unavailable",
                      {{"resource", resource},
                       {"uri", DiagnosticDetail(uri)},
                       {"robot_name", robot_name},
                       {"robot_id", robot_id},
                       {"reason", "data_unavailable"},
                       {"detail", DiagnosticDetail(error.what())},
                       {"duration_ms", std::chrono::duration<double, std::milli>(
                                           std::chrono::steady_clock::now() - started)
                                           .count()}});
      }
      throw;
    } catch (const std::exception& error) {
      lib::LogEvent(logger, spdlog::level::err, "resource_read_failed",
                    {{"resource", resource},
                     {"uri", DiagnosticDetail(uri)},
                     {"robot_name", robot_name},
                     {"robot_id", robot_id},
                     {"reason", "projection_exception"},
                     {"detail", DiagnosticDetail(error.what())},
                     {"duration_ms", std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - started)
                                         .count()}});
      throw;
    } catch (...) {
      lib::LogEvent(logger, spdlog::level::err, "resource_read_failed",
                    {{"resource", resource},
                     {"uri", DiagnosticDetail(uri)},
                     {"robot_name", robot_name},
                     {"robot_id", robot_id},
                     {"reason", "unknown_projection_exception"},
                     {"duration_ms", std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - started)
                                         .count()}});
      throw;
    }
  };
  const std::vector<std::string> fleet_segments = SplitUri(uri);
  if (fleet_segments.size() >= 3 && fleet_segments[0] == "robots") {
    const std::string& robot_name = fleet_segments[1];
    const hardware_runtime::RobotIdentity* robot = nullptr;
    for (const hardware_runtime::RobotIdentity& candidate : registry_.robots()) {
      if (candidate.name == robot_name) {
        robot = &candidate;
        break;
      }
    }
    if (robot == nullptr) {
      lib::LogEvent(logger, spdlog::level::warn, "resource_read_rejected",
                    {{"resource", "robot"},
                     {"reason", "unknown_robot"},
                     {"uri", DiagnosticDetail(uri)},
                     {"robot_name", robot_name}});
      throw std::runtime_error("state projection: unknown robot name '" + robot_name + "'");
    }
    const std::string& robot_id = robot->id;
    if (fleet_segments.size() == 5 && fleet_segments[2] == "devices" &&
        fleet_segments[4] == "state")
      return build("device", robot_name, robot_id,
                   [&] { return BuildDeviceState(robot_id, fleet_segments[3]); });
    if (fleet_segments.size() == 4 && fleet_segments[2] == "detection" &&
        fleet_segments[3] == "state")
      return build("detection", robot_name, robot_id, [&] { return BuildDetection(robot_id); });
    if (fleet_segments.size() == 5 && fleet_segments[2] == "detection" &&
        fleet_segments[3] == "head" && fleet_segments[4] == "state")
      return build("head_detection", robot_name, robot_id,
                   [&] { return BuildHeadDetection(robot_id); });
    if (fleet_segments.size() == 6 && fleet_segments[2] == "cameras" &&
        fleet_segments[4] == "detections" && fleet_segments[5] == "state")
      return build("camera_object_detection", robot_name, robot_id,
                   [&] { return BuildCameraObjectDetection(robot_id, fleet_segments[3]); });
    if (fleet_segments.size() == 5 && fleet_segments[2] == "world" && fleet_segments[3] == "asr" &&
        fleet_segments[4] == "state")
      return build("asr", robot_name, robot_id, [&] { return BuildAsr(); });
    if (fleet_segments.size() == 5 && fleet_segments[2] == "world" && fleet_segments[3] == "vad" &&
        fleet_segments[4] == "state")
      return build("vad", robot_name, robot_id, [&] { return BuildVad(); });
    if (fleet_segments.size() == 4 && fleet_segments[2] == "arm" && fleet_segments[3] == "state")
      return build("arm", robot_name, robot_id, [&] { return BuildArmState(robot_id); });
    if (fleet_segments.size() == 4 && fleet_segments[3] == "status") {
      if (fleet_segments[2] == "base")
        return build("base_status", robot_name, robot_id,
                     [&] { return BuildBaseStatus(robot_id); });
      if (fleet_segments[2] == "arm")
        return build("arm_status", robot_name, robot_id, [&] { return BuildArmStatus(robot_id); });
      if (fleet_segments[2] == "gripper")
        return build("gripper_status", robot_name, robot_id,
                     [&] { return BuildGripperStatus(robot_id); });
    }
  }
  lib::LogEvent(logger, spdlog::level::warn, "resource_read_rejected",
                {{"resource", "unknown"},
                 {"reason", "unsupported_uri"},
                 {"uri", DiagnosticDetail(uri)},
                 {"segments", fleet_segments.size()}});
  throw std::runtime_error("state projection: unknown resource uri '" + uri +
                           "'; use robots/{robot_name}/devices/{device_id}/state, "
                           "robots/{robot_name}/detection/state, "
                           "robots/{robot_name}/detection/head/state, "
                           "robots/{robot_name}/cameras/{camera_device_id}/detections/state, "
                           "robots/{robot_name}/world/vad/state, "
                           "robots/{robot_name}/world/asr/state, or "
                           "robots/{robot_name}/arm/state, "
                           "robots/{robot_name}/base/status, "
                           "robots/{robot_name}/arm/status, or "
                           "robots/{robot_name}/gripper/status");
}

}  // namespace eos::runtime::perception::body
