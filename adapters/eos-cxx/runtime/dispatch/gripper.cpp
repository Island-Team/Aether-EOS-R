#include <chrono>

#include "common.h"
#include "dispatcher.h"

namespace eos::runtime::dispatch {
using namespace detail;
nlohmann::json Dispatcher::GripperMotion(const std::string& primitive, const std::string& version,
                                         hardware_runtime::Device& device,
                                         const nlohmann::json& arguments) {
  const auto action = arguments.find("action");
  if (action == arguments.end() || !action->is_string() ||
      (*action != "open" && *action != "close"))
    return Ack(primitive, version, arguments, false, kInvalidArgument,
               "action must be open or close");
  hardware::GripperCommand command;
  command.command_id = arguments.value("command_id", "");
  command.action =
      *action == "open" ? hardware::GripperAction::kOpen : hardware::GripperAction::kClose;
  if (arguments.contains("force_n")) {
    double force = 0.0;
    if (!InAdmittedRange(arguments, "force_n", device, primitive, &force))
      return Ack(primitive, version, arguments, false, kInvalidArgument,
                 "force_n is outside the admitted device range");
    command.force_n = force;
  }
  if (arguments.contains("opening_pct")) {
    double opening = 0.0;
    if (!InAdmittedRange(arguments, "opening_pct", device, primitive, &opening) ||
        (opening != 0.0 && opening != 100.0) ||
        (command.action == hardware::GripperAction::kOpen ? opening != 100.0 : opening != 0.0))
      return Ack(primitive, version, arguments, false, kInvalidArgument,
                 "opening_pct must match action: 100 for open, 0 for close");
    command.opening_pct = opening;
  }
  if (arguments.contains("speed_pct")) {
    double speed = 0.0;
    if (!InAdmittedRange(arguments, "speed_pct", device, primitive, &speed) || speed < 1.0 ||
        speed > 100.0)
      return Ack(primitive, version, arguments, false, kInvalidArgument,
                 "speed_pct is outside the admitted device range");
    command.speed_pct = speed;
  }
  command.stamp = std::chrono::steady_clock::now();
  return Released(primitive, version, arguments, device, device.backend().ExecuteGripper(command),
                  "gripper refused the release");
}
}  // namespace eos::runtime::dispatch
