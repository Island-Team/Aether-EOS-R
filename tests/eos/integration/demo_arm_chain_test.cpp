#include "src/eos/ext/backends/builtin.h"
#include <cassert>
#include <cmath>
#include <stdexcept>
#include <string>

#include "src/eos/lib/clock.h"
#include "src/eos/runtime/hardware/registry.h"
#include "src/eos/runtime/dispatch/dispatcher.h"
#include "src/eos/runtime/init/schemas.h"
#include "src/eos/runtime/perception/body/reader.h"

int main() {
  eos::runtime::hardware_runtime::Registry registry = eos::runtime::hardware_runtime::Registry::Load(eos::backends::BuildBuiltinBackendCatalog(),
      std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo");
  eos::runtime::init::SchemaStore schemas(EOS_CONTRACTS_ROOT);
  eos::runtime::dispatch::Dispatcher dispatch(registry, schemas);

  const auto result = dispatch.Execute("arm.movej", "1.0.0", "arm-0",
                                       {{"command_id", "demo-joint-1"},
                                        {"reference", "absolute"},
                                        {"joint_positions_rad", {0.5, 0.625, 0.5, 0.5, 0.5, 0.5}}});
  const nlohmann::json& ack = result.payload;
  assert(result.status == eos::runtime::dispatch::DispatchResult::Status::kCompleted);
  assert(ack.at("accepted").get<bool>());

  registry.Tick(1.0);
  eos::lib::SystemClock clock;
  eos::runtime::perception::body::Reader projection(registry, clock);
  const auto arm_after_movej =
      registry.Find(eos::hardware::DeviceKind::kManipulator).feeds().ReadManipulatorTelemetry();
  assert(arm_after_movej.has_value());
  assert(std::abs(arm_after_movej->joint_angles_rad.at(1) - 0.625) < 1e-9);

  const auto position_result = dispatch.Execute("arm.movel", "1.0.0", "arm-0",
                                                {{"command_id", "demo-position-1"},
                                                 {"reference", "absolute"},
                                                 {"frame_id", "base_link"},
                                                 {"position_m", {0.3, 0.05, 0.0}},
                                                 {"orientation_quat_xyzw", {0.0, 0.0, 0.0, 1.0}}});
  assert(position_result.status == eos::runtime::dispatch::DispatchResult::Status::kCompleted);
  assert(position_result.payload.at("accepted").get<bool>());
  const auto pose = registry.Find(eos::hardware::DeviceKind::kManipulator)
                        .feeds().ReadManipulatorTelemetry();
  assert(pose.has_value());
  assert(std::abs(pose->position_m[0] - 0.3) < 1e-9);
  assert(std::abs(pose->position_m[1] - 0.05) < 1e-9);
  registry.Tick(1.0);

  // Actuator telemetry remains internal: body reports only health/state, and
  // the demo does not fabricate world observations from simulated joints.
  const nlohmann::json body = projection.BuildManipulator("arm-0");
  assert(body.at("health") == "ready");
  assert(body.at("state") == "idle");
  assert(body.dump().find("joint") == std::string::npos);
}
