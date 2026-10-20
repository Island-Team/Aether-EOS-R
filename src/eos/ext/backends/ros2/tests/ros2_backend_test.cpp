/// Isolated ROS graph tests; all command receivers are in-process mocks.
#include "src/eos/ext/backends/ros2/ros2_backend.h"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <future>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/compressed_image.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/empty.hpp>
#include <std_msgs/msg/float64_multi_array.hpp>
#include <std_msgs/msg/string.hpp>
#include <thread>

#include "src/eos/ext/backends/demo/cam/demo_cam.h"

namespace {
using eos::backends::ros2::Ros2Backend;
using eos::hardware::DeviceKind;
using eos::hardware::ReleaseResult;
using eos::runtime::hardware_runtime::Ros2DeviceConfig;
using eos::runtime::hardware_runtime::Ros2GimbalJointMap;
using namespace std::chrono_literals;

class Ros2Test : public testing::Test {
 protected:
  void SetUp() override {
    context_ = std::make_shared<rclcpp::Context>();
    rclcpp::InitOptions options;
    options.set_domain_id(231);
    context_->init(0, nullptr, options);
    node_ =
        std::make_shared<rclcpp::Node>("eos_test_peer", rclcpp::NodeOptions().context(context_));
    rclcpp::ExecutorOptions executor_options;
    executor_options.context = context_;
    executor_ = std::make_unique<rclcpp::executors::SingleThreadedExecutor>(executor_options);
    executor_->add_node(node_);
  }
  void TearDown() override {
    executor_.reset();
    node_.reset();
    context_->shutdown("test complete");
  }
  bool Wait(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 2500ms) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    do {
      executor_->spin_some();
      if (condition()) return true;
      std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < end);
    return false;
  }
  Ros2DeviceConfig Config() {
    Ros2DeviceConfig config;
    config.domain_id = 231;
    config.telemetry_timeout_ms = 250;
    config.command_timeout_ms = 2500;
    config.base_frame = "base_link";
    config.joint_states.name = "/test/joints";
    config.pose_query.name = "/test/pose";
    config.arm_status.name = "/test/arm_status";
    config.movej.name = "/test/movej";
    config.movel.name = "/test/movel";
    config.pick.name = "/test/pick";
    config.command_result.name = "/test/command_result";
    config.command_cancel.name = "/test/command_cancel";
    config.stop.name = "/test/stop";
    config.estop.name = "/test/estop";
    config.gripper_state.name = "/test/gripper";
    config.gripper_command.name = "/test/gripper_command";
    config.image.name = "/test/image";
    return config;
  }
  rclcpp::Context::SharedPtr context_;
  rclcpp::Node::SharedPtr node_;
  std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> executor_;
};

TEST_F(Ros2Test, ArmStatusPreservesTypedDiagnosticsAndExpiresIndependently) {
  Ros2Backend backend("arm_status_test", Config(), DeviceKind::kManipulator,
                      {"j1", "j2", "j3", "j4", "j5", "j6"});
  auto statuses = node_->create_publisher<std_msgs::msg::String>("/test/arm_status", 10);
  ASSERT_TRUE(Wait([&] { return statuses->get_subscription_count() > 0; }));

  std_msgs::msg::String invalid;
  invalid.data =
      R"({"schema_version":"1.0","observed_at":{"sec":1,"nanosec":2},"controller":{"state":"idle","errors":[]},"joints":[{"name":"wrong"}]})";
  statuses->publish(invalid);
  EXPECT_FALSE(Wait([&] {
    const auto telemetry = backend.ReadManipulatorTelemetry();
    return telemetry && telemetry->status_valid;
  }, 50ms));

  std_msgs::msg::String status;
  status.data =
      R"({"schema_version":"1.0","observed_at":{"sec":123,"nanosec":456},"controller":{"state":"joint_motion","raw_code":2,"errors":[4105]},"joints":[{"name":"j1","enabled":true,"error_code":0,"motor_current_raw":1.0},{"name":"j2"},{"name":"j3"},{"name":"j4"},{"name":"j5"},{"name":"j6","temperature_raw":25.0,"voltage_raw":48.5}]})";
  ASSERT_TRUE(Wait([&] {
    statuses->publish(status);
    const auto telemetry = backend.ReadManipulatorTelemetry();
    return telemetry && telemetry->status_valid;
  }));
  const auto telemetry = backend.ReadManipulatorTelemetry();
  ASSERT_TRUE(telemetry);
  EXPECT_EQ(telemetry->controller_state, "joint_motion");
  ASSERT_TRUE(telemetry->controller_status_code);
  EXPECT_EQ(*telemetry->controller_status_code, 2);
  EXPECT_EQ(telemetry->controller_error_codes, (std::vector<std::int64_t>{4105}));
  EXPECT_EQ(telemetry->status_source_timestamp_ns, 123000000456LL);
  EXPECT_EQ(telemetry->joint_diagnostics[0].enabled, true);
  EXPECT_EQ(telemetry->joint_diagnostics[0].error_code, 0);
  EXPECT_EQ(telemetry->joint_diagnostics[0].motor_current_raw, 1.0);
  EXPECT_EQ(telemetry->joint_diagnostics[5].temperature_raw, 25.0);
  EXPECT_EQ(telemetry->joint_diagnostics[5].voltage_raw, 48.5);
  ASSERT_TRUE(Wait([&] {
    const auto expired = backend.ReadManipulatorTelemetry();
    return expired && !expired->status_valid;
  }));
}

TEST_F(Ros2Test, MoveJRequiresOnlyJointStreamAndKeepsArmFreshnessSeparate) {
  Ros2Backend backend("arm_test", Config(), DeviceKind::kManipulator,
                      {"j1", "j2", "j3", "j4", "j5", "j6"});
  EXPECT_FALSE(backend.ReadHealth().alive);
  eos::hardware::MoveJCommand command;
  command.joint_positions_rad = {0, 0, 0, 0, 0, 5};
  command.command_id = "movej-test";
  EXPECT_EQ(backend.ExecuteMoveJ(command), ReleaseResult::kRejected);
  auto joints = node_->create_publisher<sensor_msgs::msg::JointState>("/test/joints", 10);
  auto poses = node_->create_publisher<geometry_msgs::msg::PoseStamped>("/test/pose", 10);
  auto results = node_->create_publisher<std_msgs::msg::String>("/test/command_result", 10);
  std::string cancelled_command;
  auto cancellations = node_->create_subscription<std_msgs::msg::String>(
      "/test/command_cancel", 10,
      [&](const std_msgs::msg::String& message) { cancelled_command = message.data; });
  bool received = false;
  std::string correlation;
  auto commands = node_->create_subscription<std_msgs::msg::Float64MultiArray>(
      "/test/movej", 10, [&](const std_msgs::msg::Float64MultiArray& msg) {
        received = msg.data.size() == 7 && msg.data[5] == 5.0 && msg.data[6] == -1.0;
        correlation = msg.layout.dim.empty() ? "" : msg.layout.dim.front().label;
      });
  std::string movel_correlation;
  auto movel_commands = node_->create_subscription<std_msgs::msg::Float64MultiArray>(
      "/test/movel", 10, [&](const std_msgs::msg::Float64MultiArray& msg) {
        movel_correlation = msg.layout.dim.empty() ? "" : msg.layout.dim.front().label;
      });
  std::string picked;
  auto picks = node_->create_subscription<std_msgs::msg::String>(
      "/test/pick", 10, [&](const std_msgs::msg::String& msg) { picked = msg.data; });
  ASSERT_TRUE(
      Wait([&] { return joints->get_subscription_count() && poses->get_subscription_count(); }));
  sensor_msgs::msg::JointState joint;
  joint.name = {"j6", "j5", "j4", "j3", "j2", "j1"};
  joint.position = {5, 0, 0, 0, 0, 0.4};
  joint.header.stamp = node_->now();
  joints->publish(joint);
  ASSERT_TRUE(Wait([&] { return backend.ReadManipulatorTelemetry().has_value(); }));
  ASSERT_TRUE(Wait([&] { return results->get_subscription_count() > 0; }));
  EXPECT_FALSE(backend.ReadHealth().alive);
  auto movej = std::async(std::launch::async, [&] { return backend.ExecuteMoveJ(command); });
  ASSERT_TRUE(Wait([&] { return received; }));
  EXPECT_EQ(correlation, "command_id=movej-test;primitive=arm.movej");
  std_msgs::msg::String succeeded;
  succeeded.data = R"({"command_id":"movej-test","status":"succeeded"})";
  results->publish(succeeded);
  EXPECT_EQ(movej.get(), ReleaseResult::kAccepted);

  received = false;
  correlation.clear();
  command.command_id = "movej-stream-test";
  command.stream = true;
  EXPECT_EQ(backend.ExecuteMoveJ(command), ReleaseResult::kAccepted);
  ASSERT_TRUE(Wait([&] { return received; }));
  EXPECT_EQ(correlation, "command_id=movej-stream-test;primitive=arm.movej;stream=true");
  succeeded.data = R"({"command_id":"movej-stream-test","status":"failed"})";
  results->publish(succeeded);

  EXPECT_FALSE(Wait([&] { return false; }, 50ms));
  received = false;
  command.stream = false;
  auto after_stream = std::async(std::launch::async, [&] { return backend.ExecuteMoveJ(command); });
  ASSERT_TRUE(Wait([&] { return received; }));
  EXPECT_EQ(after_stream.wait_for(50ms), std::future_status::timeout);
  succeeded.data = R"({"command_id":"movej-stream-test","status":"succeeded"})";
  results->publish(succeeded);
  EXPECT_EQ(after_stream.get(), ReleaseResult::kAccepted);

  geometry_msgs::msg::PoseStamped pose;
  pose.header.frame_id = "wrong_frame";
  pose.pose.orientation.w = 1;
  pose.header.stamp = node_->now();
  poses->publish(pose);
  EXPECT_FALSE(Wait([&] { return backend.ReadManipulatorTelemetry()->pose_valid; }, 50ms));
  pose.header.frame_id = "base_link";
  ASSERT_TRUE(Wait([&] {
    pose.header.stamp = node_->now();
    joint.header.stamp = node_->now();
    poses->publish(pose);
    joints->publish(joint);
    return backend.ReadManipulatorTelemetry()->pose_valid;
  }));
  eos::hardware::MoveLCommand movel_command;
  movel_command.position_m[0] = 0.2;
  movel_command.orientation_quat_xyzw[3] = 1.0;
  movel_command.command_id = "movel-test";
  auto movel = std::async(std::launch::async, [&] { return backend.ExecuteMoveL(movel_command); });
  ASSERT_TRUE(Wait([&] { return !movel_correlation.empty(); }));
  EXPECT_EQ(movel_correlation, "command_id=movel-test;primitive=arm.movel");
  succeeded.data = R"({"command_id":"movel-test","status":"succeeded"})";
  results->publish(succeeded);
  EXPECT_EQ(movel.get(), ReleaseResult::kAccepted);

  movel_correlation.clear();
  movel_command.command_id = "movel-stream-test";
  movel_command.stream = true;
  EXPECT_EQ(backend.ExecuteMoveL(movel_command), ReleaseResult::kAccepted);
  ASSERT_TRUE(Wait([&] { return !movel_correlation.empty(); }));
  EXPECT_EQ(movel_correlation, "command_id=movel-stream-test;primitive=arm.movel;stream=true");

  eos::hardware::PickCommand pick;
  pick.object_id = "obj:part-7";
  pick.command_id = "pick-test";
  auto pick_result = std::async(std::launch::async, [&] { return backend.ExecutePick(pick); });
  ASSERT_TRUE(Wait([&] { return picked.find("obj:part-7") != std::string::npos; }));
  succeeded.data = R"({"command_id":"pick-test","status":"succeeded"})";
  results->publish(succeeded);
  EXPECT_EQ(pick_result.get(), ReleaseResult::kAccepted);

  received = false;
  command.command_id = "movej-cancel-test";
  auto cancelled_movej =
      std::async(std::launch::async, [&] { return backend.ExecuteMoveJ(command); });
  ASSERT_TRUE(Wait([&] { return received && cancellations->get_publisher_count() > 0; }));
  EXPECT_EQ(backend.CancelCommand(command.command_id), ReleaseResult::kAccepted);
  ASSERT_TRUE(Wait([&] { return cancelled_command == command.command_id; }));
  succeeded.data = R"({"command_id":"movej-cancel-test","status":"cancelled"})";
  results->publish(succeeded);
  EXPECT_EQ(cancelled_movej.get(), ReleaseResult::kCancelled);
  EXPECT_DOUBLE_EQ(backend.ReadManipulatorTelemetry()->joint_angles_rad[0], 0.4);
  ASSERT_TRUE(Wait([&] {
    joint.header.stamp = node_->now();
    joints->publish(joint);
    return !backend.ReadManipulatorTelemetry()->pose_valid;
  }));
  EXPECT_FALSE(backend.ReadHealth().alive);
  EXPECT_TRUE(backend.ReadManipulatorTelemetry()->joint_valid[0]);
}

TEST_F(Ros2Test, StopRemainsAvailableWhileTelemetryIsUnhealthy) {
  Ros2Backend backend("arm_stop_test", Config(), DeviceKind::kManipulator,
                      {"j1", "j2", "j3", "j4", "j5", "j6"});
  bool stopped = false;
  auto stop = node_->create_subscription<std_msgs::msg::Empty>(
      "/test/stop", 10, [&](const std_msgs::msg::Empty&) { stopped = true; });
  ASSERT_TRUE(Wait([&] { return backend.StopMotion() == ReleaseResult::kAccepted; }));
  EXPECT_TRUE(Wait([&] { return stopped; }));
}

TEST_F(Ros2Test, GimbalPreservesArmJointsAndInvertsTiltAtJoint5Boundary) {
  auto config = Config();
  config.health.name = "/test/health";
  config.gimbal_joint_map = Ros2GimbalJointMap{"default", "j4", "j5", -1};
  Ros2Backend backend("gimbal_test", config, DeviceKind::kGimbal,
                      {"j1", "j2", "j3", "j4", "j5", "j6"});

  auto joints = node_->create_publisher<sensor_msgs::msg::JointState>("/test/joints", 10);
  auto health = node_->create_publisher<std_msgs::msg::Bool>("/test/health", 10);
  auto results = node_->create_publisher<std_msgs::msg::String>("/test/command_result", 10);
  std::vector<double> received;
  std::string correlation;
  auto commands = node_->create_subscription<std_msgs::msg::Float64MultiArray>(
      "/test/movej", 10, [&](const std_msgs::msg::Float64MultiArray& message) {
        received = message.data;
        correlation = message.layout.dim.empty() ? "" : message.layout.dim.front().label;
      });
  ASSERT_TRUE(Wait([&] {
    return joints->get_subscription_count() && health->get_subscription_count() &&
           commands->get_publisher_count();
  }));

  sensor_msgs::msg::JointState state;
  state.name = {"j6", "j4", "j2", "j5", "j1", "j3"};
  state.position = {0.6, 0.4, 0.2, 0.5, 0.1, 0.3};
  state.velocity = {6.0, 4.0, 2.0, 5.0, 1.0, 3.0};
  std_msgs::msg::Bool alive;
  alive.data = true;
  ASSERT_TRUE(Wait([&] {
    state.header.stamp = node_->now();
    joints->publish(state);
    health->publish(alive);
    return backend.ReadHealth().alive;
  }));

  const auto telemetry = backend.ReadGimbalTelemetry();
  ASSERT_TRUE(telemetry);
  EXPECT_DOUBLE_EQ(telemetry->pan_rad, 0.4);
  EXPECT_DOUBLE_EQ(telemetry->tilt_rad, -0.5);
  EXPECT_DOUBLE_EQ(telemetry->pan_rate_rad_s, 4.0);
  EXPECT_DOUBLE_EQ(telemetry->tilt_rate_rad_s, -5.0);
  ASSERT_TRUE(Wait([&] { return results->get_subscription_count() > 0; }));

  eos::hardware::GimbalCommand command;
  command.pan_rad = -0.8;
  command.tilt_rad = 0.9;
  command.command_id = "gimbal-test";
  auto gimbal = std::async(std::launch::async, [&] { return backend.ExecuteGimbal(command); });
  ASSERT_TRUE(Wait([&] { return !received.empty(); }));
  std_msgs::msg::String succeeded;
  succeeded.data = R"({"command_id":"gimbal-test","status":"succeeded"})";
  results->publish(succeeded);
  EXPECT_EQ(gimbal.get(), ReleaseResult::kAccepted);
  EXPECT_EQ(received, (std::vector<double>{0.1, 0.2, 0.3, -0.8, -0.9, 0.6, -1.0}));
  EXPECT_EQ(correlation, "command_id=gimbal-test;primitive=gimbal.motion");

  received.clear();
  correlation.clear();
  command.command_id = "gimbal-stream-test";
  command.stream = true;
  EXPECT_EQ(backend.ExecuteGimbal(command), ReleaseResult::kAccepted);
  ASSERT_TRUE(Wait([&] { return !received.empty(); }));
  EXPECT_EQ(correlation, "command_id=gimbal-stream-test;primitive=gimbal.motion;stream=true");
}

TEST_F(Ros2Test, GripperForceIsUnknownAndFaultHeartbeatBlocksCommands) {
  auto config = Config();
  config.health.name = "/test/health";
  Ros2Backend backend("gripper_test", config, DeviceKind::kGripper);
  auto states = node_->create_publisher<std_msgs::msg::Float64MultiArray>("/test/gripper", 10);
  auto health = node_->create_publisher<std_msgs::msg::Bool>("/test/health", 10);
  ASSERT_TRUE(
      Wait([&] { return states->get_subscription_count() && health->get_subscription_count(); }));
  std_msgs::msg::Float64MultiArray state;
  state.data = {50, 0, -1};
  std_msgs::msg::Bool alive;
  alive.data = true;
  ASSERT_TRUE(Wait([&] {
    states->publish(state);
    health->publish(alive);
    return backend.ReadHealth().alive;
  }));
  const auto telemetry = backend.ReadGripperTelemetry();
  ASSERT_TRUE(telemetry);
  EXPECT_FALSE(telemetry->force_n);
  EXPECT_EQ(telemetry->state, eos::hardware::GripperState::kUnknown);
  alive.data = false;
  ASSERT_TRUE(Wait([&] {
    health->publish(alive);
    return !backend.ReadHealth().alive;
  }));
  ASSERT_TRUE(Wait([&] { return !backend.ReadGripperTelemetry(); }));
}

TEST_F(Ros2Test, CameraRejectsStaleAndOversizedFramesThenExpiresRealJpeg) {
  auto config = Config();
  config.max_image_bytes = 200;
  Ros2Backend backend("camera_test", config, DeviceKind::kCamera);
  auto images = node_->create_publisher<sensor_msgs::msg::CompressedImage>("/test/image",
                                                                           rclcpp::SensorDataQoS());
  ASSERT_TRUE(Wait([&] { return images->get_subscription_count() > 0; }));
  sensor_msgs::msg::CompressedImage image;
  image.header.frame_id = "camera_link";
  image.format = "jpeg";
  eos::backends::demo::DemoCam demo("jpeg_fixture");
  image.data = demo.ReadCameraTelemetry()->jpeg;
  images->publish(image);  // Zero stamp cannot revive a stale cached image.
  EXPECT_FALSE(Wait([&] { return backend.ReadCameraTelemetry().has_value(); }, 50ms));
  image.header.stamp = node_->now();
  image.data.resize(201);
  images->publish(image);
  EXPECT_FALSE(Wait([&] { return backend.ReadCameraTelemetry().has_value(); }, 50ms));
  image.data = demo.ReadCameraTelemetry()->jpeg;
  image.data.pop_back();
  image.header.stamp = node_->now();
  images->publish(image);
  EXPECT_FALSE(Wait([&] { return backend.ReadCameraTelemetry().has_value(); }, 50ms));
  image.data = demo.ReadCameraTelemetry()->jpeg;
  ASSERT_TRUE(Wait([&] {
    image.header.stamp = node_->now();
    images->publish(image);
    return backend.ReadCameraTelemetry().has_value();
  }));
  const auto frame = backend.ReadCameraTelemetry();
  EXPECT_EQ(frame->jpeg, image.data);
  EXPECT_EQ(frame->width_px, 1U);
  EXPECT_EQ(frame->frame_id, "camera_link");
  ASSERT_TRUE(frame->source_timestamp_ns);
  ASSERT_TRUE(Wait([&] { return !backend.ReadCameraTelemetry(); }));
  EXPECT_FALSE(backend.ReadHealth().alive);
}

TEST_F(Ros2Test, CameraClampIsNoOpAndHaltLatchesAvailability) {
  Ros2Backend backend("camera_safety_test", Config(), DeviceKind::kCamera);

  EXPECT_FALSE(backend.ReadHealth().alive);
  backend.Clamp();
  EXPECT_FALSE(backend.ReadHealth().alive);

  backend.Halt();
  EXPECT_FALSE(backend.ReadHealth().alive);
  backend.Halt();
  EXPECT_FALSE(backend.ReadHealth().alive);
}

TEST_F(Ros2Test, CameraPairsSameTimestampRgbAndAlignedDepth) {
  auto config = Config();
  config.depth_image.name = "/test/depth";
  config.max_image_bytes = 4096;
  config.depth_scale = 0.001;
  Ros2Backend backend("depth_camera_test", config, DeviceKind::kCamera);
  auto images = node_->create_publisher<sensor_msgs::msg::CompressedImage>("/test/image",
                                                                           rclcpp::SensorDataQoS());
  auto depths =
      node_->create_publisher<sensor_msgs::msg::Image>("/test/depth", rclcpp::SensorDataQoS());
  ASSERT_TRUE(Wait([&] {
    return images->get_subscription_count() > 0 && depths->get_subscription_count() > 0;
  }));

  eos::backends::demo::DemoCam demo("depth_fixture");
  sensor_msgs::msg::CompressedImage image;
  image.header.frame_id = "camera_link";
  image.format = "jpeg";
  image.data = demo.ReadCameraTelemetry()->jpeg;
  sensor_msgs::msg::Image depth;
  depth.header.frame_id = "camera_link";
  depth.height = 1;
  depth.width = 1;
  depth.encoding = "16UC1";
  depth.is_bigendian = false;
  depth.step = 2;
  depth.data = {0xe8, 0x03};

  ASSERT_TRUE(Wait([&] {
    const auto stamp = node_->now();
    image.header.stamp = stamp;
    depth.header.stamp = stamp;
    images->publish(image);
    depths->publish(depth);
    const auto frame = backend.ReadCameraTelemetry();
    return frame.has_value() && !frame->depth_u16.empty();
  }));
  const auto frame = backend.ReadCameraTelemetry();
  ASSERT_TRUE(frame);
  ASSERT_EQ(frame->depth_u16.size(), 1U);
  EXPECT_EQ(frame->depth_u16[0], 1000U);
  EXPECT_DOUBLE_EQ(frame->depth_scale, 0.001);
}
}  // namespace
