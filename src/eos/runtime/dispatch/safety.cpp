#include <exception>
#include <mutex>
#include <thread>

#include "common.h"
#include "dispatcher.h"
#include "src/eos/lib/log.h"

namespace eos::runtime::dispatch {
using namespace detail;

nlohmann::json Dispatcher::MotionStop(const std::string& primitive, const std::string& version,
                                      hardware_runtime::Device& device,
                                      const nlohmann::json& arguments) {
  const hardware::ReleaseResult released = device.backend().StopMotion();
  if (released != hardware::ReleaseResult::kAccepted) {
    return Released(primitive, version, arguments, device, released,
                    "motion stop was not released");
  }
  const std::string device_id = device.backend().Identify().device_id;
  bool lease_cleared = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    lease_cleared = base_velocity_lease_s_.erase(device_id) != 0;
    gimbal_stream_targets_.erase(device_id);
  }
  lib::LogEvent(lib::GetLogger("dispatch"),
                lease_cleared ? spdlog::level::info : spdlog::level::debug, "motion_stopped",
                {{"device_id", device_id}, {"lease_cleared", lease_cleared}});
  return Accepted(primitive, version, arguments);
}

nlohmann::json Dispatcher::RobotStop(const std::string& primitive, const std::string& version,
                                     const nlohmann::json& arguments) {
  struct StopOutcome {
    std::string device_id;
    std::string status{"failed"};
    std::string reason;
  };

  std::vector<hardware_runtime::Device*> targets;
  for (hardware_runtime::Device& device : registry_.devices_) {
    if (IsMotion(device.identity().kind)) targets.push_back(&device);
  }
  std::vector<StopOutcome> outcomes(targets.size());
  std::vector<std::thread> stops;
  stops.reserve(targets.size());
  const std::string correlation_id = lib::CurrentCorrelationId();
  auto stop_one = [this, &outcomes, &targets, correlation_id](std::size_t index) {
    hardware_runtime::Device& device = *targets[index];
    StopOutcome& outcome = outcomes[index];
    outcome.device_id = device.identity().device_id;
    std::optional<lib::CorrelationScope> correlation;
    if (!correlation_id.empty()) correlation.emplace(correlation_id);
    try {
      std::unique_lock<std::mutex> lock;
      if (!device.backend().CanStopWithoutDeviceLock())
        lock = std::unique_lock<std::mutex>(*DeviceMutex(outcome.device_id));
      const hardware::ReleaseResult released = device.backend().StopMotion();
      switch (released) {
        case hardware::ReleaseResult::kAccepted:
          outcome.status = "succeeded";
          break;
        case hardware::ReleaseResult::kTimedOut:
          outcome.status = "timed_out";
          break;
        case hardware::ReleaseResult::kCancelled:
          outcome.status = "cancelled";
          break;
        case hardware::ReleaseResult::kOutcomeUnknown:
          outcome.status = "outcome_unknown";
          break;
        case hardware::ReleaseResult::kFailed:
        case hardware::ReleaseResult::kRejected:
          outcome.status = "failed";
          break;
      }
      if (outcome.status != "succeeded") {
        outcome.reason = device.backend().ReleaseFailureReason();
        if (outcome.reason.empty()) outcome.reason = "device stop was not confirmed";
      }
    } catch (const std::exception& error) {
      outcome.reason = error.what();
    } catch (...) {
      outcome.reason = "unknown backend exception";
    }
  };

  std::size_t scheduled = 0;
  try {
    for (; scheduled < targets.size(); ++scheduled) {
      stops.emplace_back(stop_one, scheduled);
    }
  } catch (const std::exception& error) {
    lib::LogEvent(lib::GetLogger("dispatch"), spdlog::level::warn,
                  "robot_stop_parallelism_degraded",
                  {{"scheduled", scheduled}, {"detail", error.what()}});
  }
  for (std::thread& stop : stops) stop.join();
  for (; scheduled < targets.size(); ++scheduled) stop_one(scheduled);

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (const StopOutcome& outcome : outcomes) {
      if (outcome.status == "succeeded") {
        base_velocity_lease_s_.erase(outcome.device_id);
        gimbal_stream_targets_.erase(outcome.device_id);
      }
    }
  }

  nlohmann::json ack = Accepted(primitive, version, arguments);
  nlohmann::json parts = nlohmann::json::array();
  std::string aggregate = "succeeded";
  for (const StopOutcome& outcome : outcomes) {
    nlohmann::json part{{"device_id", outcome.device_id}, {"status", outcome.status}};
    if (!outcome.reason.empty()) part["reason"] = outcome.reason;
    parts.push_back(std::move(part));
    if (outcome.status == "outcome_unknown") {
      aggregate = "outcome_unknown";
    } else if (outcome.status == "timed_out" && aggregate != "outcome_unknown") {
      aggregate = "timed_out";
    } else if (outcome.status != "succeeded" && aggregate == "succeeded") {
      aggregate = "failed";
    }
  }
  ack["result"] = {{"status", aggregate}, {"parts", std::move(parts)}};
  if (aggregate != "succeeded") {
    ack["result"]["reason"] = "one or more robot parts did not confirm a controlled stop";
  }
  lib::LogEvent(lib::GetLogger("dispatch"),
                aggregate == "succeeded" ? spdlog::level::info : spdlog::level::err,
                "robot_stop_completed", {{"status", aggregate}, {"devices", targets.size()}});
  return ack;
}

nlohmann::json Dispatcher::Estop(const std::string& primitive, const std::string& version,
                                 const nlohmann::json& arguments) {
  lib::LogEvent(lib::GetLogger("dispatch"), spdlog::level::warn, "estop_requested",
                {{"devices", registry_.devices_.size()}});
  registry_.estop_latched_.store(true);
  const std::size_t halted_devices = registry_.devices_.size();
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    base_velocity_lease_s_.clear();
    gimbal_stream_targets_.clear();
  }
  std::vector<std::thread> halts;
  halts.reserve(registry_.devices_.size());
  const std::string correlation_id = lib::CurrentCorrelationId();
  std::size_t scheduled = 0;
  try {
    for (; scheduled < registry_.devices_.size(); ++scheduled) {
      hardware_runtime::Device* device = &registry_.devices_[scheduled];
      halts.emplace_back([this, device, correlation_id] {
        if (correlation_id.empty()) {
          HaltDevice(*device);
          return;
        }
        lib::CorrelationScope correlation(correlation_id);
        HaltDevice(*device);
      });
    }
  } catch (const std::exception& error) {
    lib::LogEvent(lib::GetLogger("dispatch"), spdlog::level::warn, "estop_parallelism_degraded",
                  {{"scheduled", scheduled}, {"detail", error.what()}});
  }
  for (std::thread& halt : halts) halt.join();
  for (; scheduled < registry_.devices_.size(); ++scheduled) {
    HaltDevice(registry_.devices_[scheduled]);
  }
  lib::LogEvent(lib::GetLogger("dispatch"), spdlog::level::critical, "estop_latched",
                {{"devices", halted_devices}});
  return Accepted(primitive, version, arguments);
}

void Dispatcher::HaltDevice(hardware_runtime::Device& device) {
  const std::string device_id = device.identity().device_id;
  std::unique_lock<std::mutex> lock;
  if (!device.backend().CanStopWithoutDeviceLock())
    lock = std::unique_lock<std::mutex>(*DeviceMutex(device_id));
  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    gimbal_stream_targets_.erase(device_id);
  }
  try {
    device.backend().Halt();
  } catch (const std::exception& error) {
    lib::LogEvent(lib::GetLogger("hardware"), spdlog::level::err, "backend_safety_failed",
                  {{"device_id", device_id}, {"operation", "halt"}, {"detail", error.what()}});
  } catch (...) {
    lib::LogEvent(
        lib::GetLogger("hardware"), spdlog::level::err, "backend_safety_failed",
        {{"device_id", device_id}, {"operation", "halt"}, {"detail", "unknown backend exception"}});
  }
}

void Dispatcher::Tick(double dt_s) {
  if (dt_s <= 0.0) return;
  std::vector<std::string> expired;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (auto& [device_id, remaining_s] : base_velocity_lease_s_) {
      remaining_s -= dt_s;
      if (remaining_s <= 0.0) expired.push_back(device_id);
    }
  }
  for (const std::string& device_id : expired) {
    if (hardware_runtime::Device* device = registry_.FindDeviceForRelease(device_id)) {
      std::unique_lock<std::mutex> device_lock(*DeviceMutex(device_id));
      {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        const auto lease = base_velocity_lease_s_.find(device_id);
        if (lease == base_velocity_lease_s_.end() || lease->second > 0.0) continue;
        base_velocity_lease_s_.erase(lease);
      }
      try {
        device->backend().Clamp();
        lib::LogEvent(
            lib::GetLogger("dispatch"), spdlog::level::warn, "velocity_lease_expired",
            {{"device_id", device_id}, {"action", "clamp"}, {"cause", "velocity_lease_expired"}});
      } catch (const std::exception& error) {
        lib::LogEvent(lib::GetLogger("dispatch"), spdlog::level::err, "lease_clamp_failed",
                      {{"device_id", device_id},
                       {"cause", "velocity_lease_expired"},
                       {"detail", error.what()}});
      } catch (...) {
        lib::LogEvent(lib::GetLogger("dispatch"), spdlog::level::err, "lease_clamp_failed",
                      {{"device_id", device_id},
                       {"cause", "velocity_lease_expired"},
                       {"detail", "unknown backend exception"}});
      }
    }
  }
}
}  // namespace eos::runtime::dispatch
