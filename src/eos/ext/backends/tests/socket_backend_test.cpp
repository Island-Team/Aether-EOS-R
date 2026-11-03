#include <gtest/gtest.h>

#include "src/eos/ext/backends/ros2/socket_backend.h"

TEST(SocketBackendTest, MissingBridgeFailsClosed) {
  eos::backends::ros2::SocketBackend backend("arm-0", eos::hardware::DeviceKind::kManipulator,
                                            "/tmp/nonexistent-island-ros-bridge.sock");
  backend.MarkAdmitted();
  EXPECT_FALSE(backend.ReadHealth().alive);
  eos::hardware::MoveJCommand command;
  command.command_id = "test";
  command.joint_positions_rad = {0, 0, 0, 0, 0, 0};
  EXPECT_EQ(backend.ExecuteMoveJ(command), eos::hardware::ReleaseResult::kOutcomeUnknown);
  EXPECT_FALSE(backend.ReadManipulatorTelemetry().has_value());
  EXPECT_NE(backend.ReleaseFailureReason().find("unavailable"), std::string::npos);
}
