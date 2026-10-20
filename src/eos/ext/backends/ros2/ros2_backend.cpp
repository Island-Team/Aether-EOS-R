#include "ros2_backend.h"

#include <png.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/compressed_image.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <set>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/empty.hpp>
#include <std_msgs/msg/float64_multi_array.hpp>
#include <std_msgs/msg/string.hpp>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "src/eos/lib/log.h"

namespace eos::backends::ros2 {

struct AlignedDepthSample {
  std::uint32_t width_px{0};
  std::uint32_t height_px{0};
  std::string frame_id;
  std::int64_t source_timestamp_ns{0};
  std::vector<std::uint16_t> pixels;
};

struct Ros2Backend::Impl {
  Impl(std::string id, runtime::hardware_runtime::Ros2DeviceConfig supplied,
       hardware::DeviceKind device_kind, std::vector<std::string> names)
      : device_id(std::move(id)),
        config(std::move(supplied)),
        kind(device_kind),
        joint_names(std::move(names)) {}

  ~Impl() {
    if (started) {
      eos::lib::LogEvent(eos::lib::GetLogger("ros2"), spdlog::level::info,
                         "ros2_bridge_stop_started", {{"device", device_id}});
    }
    if (executor) executor->cancel();
    if (executor_thread.joinable()) executor_thread.join();
    if (context && context->is_valid()) context->shutdown("EOS ROS 2 backend stopped");
    if (started) {
      eos::lib::LogEvent(eos::lib::GetLogger("ros2"), spdlog::level::info, "ros2_bridge_stopped",
                         {{"device", device_id}});
    }
  }

  bool Fresh(std::chrono::steady_clock::time_point stamp) const {
    return stamp.time_since_epoch().count() != 0 &&
           std::chrono::steady_clock::now() - stamp <=
               std::chrono::milliseconds(config.telemetry_timeout_ms);
  }

  bool Ready() const {
    if (!Alive() || !Fresh(last_telemetry)) return false;
    if (kind == hardware::DeviceKind::kManipulator) return Fresh(last_joint) && Fresh(last_pose);
    if (kind == hardware::DeviceKind::kGimbal) return Fresh(last_joint);
    return true;
  }

  bool Alive() const {
    if (halted || executor_failed) return false;
    return config.health.name.empty() || (adapter_alive && Fresh(last_health));
  }

  std::string device_id;
  runtime::hardware_runtime::Ros2DeviceConfig config;
  hardware::DeviceKind kind;
  std::vector<std::string> joint_names;
  rclcpp::Context::SharedPtr context;
  rclcpp::Node::SharedPtr node;
  std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> executor;
  std::thread executor_thread;
  rclcpp::Publisher<std_msgs::msg::Float64MultiArray>::SharedPtr movej_pub;
  rclcpp::Publisher<std_msgs::msg::Float64MultiArray>::SharedPtr movel_pub;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr pick_pub;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr command_cancel_pub;
  rclcpp::Publisher<std_msgs::msg::Float64MultiArray>::SharedPtr gripper_command_pub;
  rclcpp::Publisher<std_msgs::msg::Empty>::SharedPtr stop_pub;
  rclcpp::Publisher<std_msgs::msg::Empty>::SharedPtr estop_pub;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr joint_states_sub;
  rclcpp::Subscription<geometry_msgs::msg::PoseStamped>::SharedPtr pose_query_sub;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr arm_status_sub;
  rclcpp::Subscription<std_msgs::msg::Float64MultiArray>::SharedPtr gripper_state_sub;
  rclcpp::Subscription<sensor_msgs::msg::CompressedImage>::SharedPtr image_sub;
  rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr depth_image_sub;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr health_sub;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr command_result_sub;
  bool adapter_alive{false};
  bool health_seen{false};
  std::chrono::steady_clock::time_point last_health{};
  mutable std::mutex state_mutex;
  std::condition_variable command_changed;
  std::map<std::string, std::pair<std::string, std::string>> command_results;
  std::set<std::string> active_commands;
  std::string last_command_reason;
  std::optional<hardware::ManipulatorTelemetry> manipulator_telemetry;
  std::optional<hardware::GripperTelemetry> gripper_telemetry;
  std::optional<hardware::CameraTelemetry> camera_telemetry;
  std::optional<hardware::CameraTelemetry> pending_color;
  std::optional<AlignedDepthSample> pending_depth;
  std::chrono::steady_clock::time_point last_telemetry{};
  std::chrono::steady_clock::time_point last_joint{}, last_pose{}, last_arm_status{};
  std::size_t gimbal_pan_index{0};
  std::size_t gimbal_tilt_index{0};
  int gimbal_tilt_direction{1};
  bool halted{false};
  bool executor_failed{false};
  bool started{false};
};

template <typename Message>
void Correlate(Message* message, const std::string& command_id, const char* primitive,
               bool stream = false) {
  std_msgs::msg::MultiArrayDimension correlation;
  correlation.label = "command_id=" + command_id + ";primitive=" + primitive;
  if (stream) correlation.label += ";stream=true";
  correlation.size = static_cast<std::uint32_t>(message->data.size());
  correlation.stride = correlation.size;
  message->layout.dim.push_back(std::move(correlation));
}

template <typename State>
hardware::ReleaseResult WaitForCommandResult(State* state, const std::string& command_id,
                                             std::uint64_t timeout_ms = 0) {
  std::unique_lock<std::mutex> lock(state->state_mutex);
  const bool received = state->command_changed.wait_for(
      lock,
      std::chrono::milliseconds(timeout_ms == 0 ? state->config.command_timeout_ms : timeout_ms),
      [&] {
        return state->halted || state->executor_failed || state->command_results.count(command_id);
      });
  if (!received || state->halted || state->executor_failed) {
    state->active_commands.erase(command_id);
    if (state->halted) {
      state->last_command_reason = "ROS 2 backend halted before terminal result";
      return hardware::ReleaseResult::kCancelled;
    }
    state->last_command_reason = !received
                                     ? "ROS 2 command result deadline expired; outcome is unknown"
                                     : "ROS 2 executor failed after command publication";
    return hardware::ReleaseResult::kOutcomeUnknown;
  }
  const auto result = state->command_results.at(command_id);
  state->command_results.erase(command_id);
  state->active_commands.erase(command_id);
  state->last_command_reason = result.second;
  if (result.first == "succeeded") return hardware::ReleaseResult::kAccepted;
  if (result.first == "failed") return hardware::ReleaseResult::kFailed;
  if (result.first == "cancelled") return hardware::ReleaseResult::kCancelled;
  if (result.first == "timed_out") return hardware::ReleaseResult::kTimedOut;
  return hardware::ReleaseResult::kOutcomeUnknown;
}

template <typename State>
void PrepareCommand(State* state, const std::string& command_id) {
  std::lock_guard<std::mutex> lock(state->state_mutex);
  state->command_results.erase(command_id);
  state->active_commands.insert(command_id);
  state->last_command_reason.clear();
}

template <typename State>
void StartCommandEndpoints(State* state) {
  state->command_cancel_pub = state->node->template create_publisher<std_msgs::msg::String>(
      state->config.command_cancel.name, 10);
  state->command_result_sub = state->node->template create_subscription<std_msgs::msg::String>(
      state->config.command_result.name, 10,
      [state](const std_msgs::msg::String::SharedPtr message) {
        try {
          const nlohmann::json result = nlohmann::json::parse(message->data);
          const std::string command_id = result.at("command_id").get<std::string>();
          const std::string status = result.at("status").get<std::string>();
          if (command_id.empty() ||
              (status != "succeeded" && status != "failed" && status != "cancelled" &&
               status != "timed_out" && status != "outcome_unknown")) {
            return;
          }
          {
            std::lock_guard<std::mutex> lock(state->state_mutex);
            if (state->active_commands.count(command_id) == 0) return;
            state->command_results[command_id] = {status, result.value("reason", "")};
          }
          eos::lib::LogEvent(
              eos::lib::GetLogger("ros2"), spdlog::level::info, "ros2_command_result_received",
              {{"device", state->device_id}, {"command_id", command_id}, {"status", status}});
          state->command_changed.notify_all();
        } catch (const std::exception&) {
          eos::lib::LogEvent(eos::lib::GetLogger("ros2"), spdlog::level::warn,
                             "ros2_command_result_rejected",
                             {{"device", state->device_id}, {"reason", "invalid_json"}});
        }
      });
}

namespace {

std::string NodeName(const std::string& device_id) {
  std::string name = "eos_ros2_" + device_id;
  for (char& character : name) {
    if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_') {
      character = '_';
    }
  }
  return name;
}

bool ValidQuaternion(const double quaternion[4]) {
  const double norm = quaternion[0] * quaternion[0] + quaternion[1] * quaternion[1] +
                      quaternion[2] * quaternion[2] + quaternion[3] * quaternion[3];
  return std::isfinite(norm) && norm > 0.999 && norm < 1.001;
}

bool ValidPositive(const std::optional<double>& value) {
  return !value || (std::isfinite(*value) && *value > 0.0);
}

const char* KindName(hardware::DeviceKind kind) {
  switch (kind) {
    case hardware::DeviceKind::kManipulator:
      return "arm";
    case hardware::DeviceKind::kGripper:
      return "gripper";
    case hardware::DeviceKind::kCamera:
      return "camera";
    case hardware::DeviceKind::kGimbal:
      return "gimbal";
    default:
      return "unsupported";
  }
}

template <typename State>
std::string ReadinessReason(const State* state, bool require_manipulator_pose = true) {
  if (state->halted) return "halted";
  if (state->executor_failed) return "executor_failed";
  if (!state->config.health.name.empty()) {
    if (!state->health_seen) return "adapter_health_missing";
    if (!state->adapter_alive) return "adapter_unhealthy";
    if (!state->Fresh(state->last_health)) return "adapter_health_stale";
  }
  if (!state->Fresh(state->last_telemetry)) return "telemetry_missing_or_stale";
  if (state->kind == hardware::DeviceKind::kManipulator) {
    if (!state->Fresh(state->last_joint)) return "joint_telemetry_missing_or_stale";
    if (require_manipulator_pose && !state->Fresh(state->last_pose))
      return "pose_telemetry_missing_or_stale";
  }
  return "ready";
}

template <typename State>
void LogCommandRejected(const State* state, const char* command, const char* reason,
                        std::size_t subscribers = 0) {
  eos::lib::LogEvent(eos::lib::GetLogger("ros2"), spdlog::level::warn, "ros2_command_rejected",
                     {{"device", state->device_id},
                      {"command", command},
                      {"reason", reason},
                      {"subscribers", subscribers}});
}

/// Extracts JPEG dimensions from a Start Of Frame marker without decoding the
/// image. Returns false for malformed or unsupported data.
bool JpegDimensions(const std::vector<std::uint8_t>& jpeg, std::uint32_t* width,
                    std::uint32_t* height) {
  if (jpeg.size() < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8 || jpeg[jpeg.size() - 2] != 0xff ||
      jpeg.back() != 0xd9)
    return false;
  bool has_frame = false;
  std::size_t offset = 2;
  while (offset + 3 < jpeg.size()) {
    if (jpeg[offset] != 0xff) {
      return false;
    }
    while (offset < jpeg.size() && jpeg[offset] == 0xff) ++offset;
    if (offset >= jpeg.size()) return false;
    const std::uint8_t marker = jpeg[offset++];
    if (marker == 0xd8 || marker == 0xd9 || (marker >= 0xd0 && marker <= 0xd7)) return false;
    if (offset + 1 >= jpeg.size()) return false;
    const std::size_t length = (static_cast<std::size_t>(jpeg[offset]) << 8) | jpeg[offset + 1];
    if (length < 2 || offset + length > jpeg.size()) return false;
    const bool start_of_frame =
        (marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
        (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf);
    if (start_of_frame) {
      if (length < 8 || length != 8U + 3U * jpeg[offset + 7]) return false;
      *height = (static_cast<std::uint32_t>(jpeg[offset + 3]) << 8) | jpeg[offset + 4];
      *width = (static_cast<std::uint32_t>(jpeg[offset + 5]) << 8) | jpeg[offset + 6];
      has_frame = *width > 0 && *height > 0;
    }
    // Check the scan header and framing. Entropy decoding is left to consumers.
    if (marker == 0xda) return has_frame && length >= 6 && offset + length < jpeg.size() - 2;
    offset += length;
  }
  return false;
}

std::vector<std::uint16_t> CopyDepthPixels(const sensor_msgs::msg::Image& depth) {
  if ((depth.encoding != "16UC1" && depth.encoding != "mono16") || depth.width == 0 ||
      depth.height == 0 || depth.step < static_cast<std::size_t>(depth.width) * 2 ||
      depth.data.size() < static_cast<std::size_t>(depth.step) * depth.height) {
    return {};
  }
  const std::size_t pixel_count = static_cast<std::size_t>(depth.width) * depth.height;
  std::vector<std::uint16_t> pixels(pixel_count);
  for (std::uint32_t y = 0; y < depth.height; ++y) {
    const std::uint8_t* row = depth.data.data() + static_cast<std::size_t>(y) * depth.step;
    for (std::uint32_t x = 0; x < depth.width; ++x) {
      const std::uint8_t* sample = row + static_cast<std::size_t>(x) * 2;
      pixels[static_cast<std::size_t>(y) * depth.width + x] =
          depth.is_bigendian ? static_cast<std::uint16_t>(sample[0] << 8 | sample[1])
                             : static_cast<std::uint16_t>(sample[0] | sample[1] << 8);
    }
  }
  return pixels;
}

template <typename State>
void PublishAlignedCameraPair(State* state) {
  if (!state->pending_color || !state->pending_depth) return;
  const auto& color = *state->pending_color;
  const auto& depth = *state->pending_depth;
  if (!color.source_timestamp_ns || *color.source_timestamp_ns != depth.source_timestamp_ns ||
      color.frame_id != depth.frame_id || color.width_px != depth.width_px ||
      color.height_px != depth.height_px) {
    return;
  }
  hardware::CameraTelemetry paired = std::move(*state->pending_color);
  paired.depth_u16 = std::move(state->pending_depth->pixels);
  paired.depth_scale = state->config.depth_scale;
  state->camera_telemetry = std::move(paired);
  state->pending_color.reset();
  state->pending_depth.reset();
  state->last_telemetry = std::chrono::steady_clock::now();
}

template <typename State>
bool FreshHeader(State* state, const std_msgs::msg::Header& header) {
  const rclcpp::Time stamp(header.stamp);
  const auto age = state->node->now().nanoseconds() - stamp.nanoseconds();
  return stamp.nanoseconds() > 0 && age >= -100000000LL &&
         age <= static_cast<std::int64_t>(state->config.telemetry_timeout_ms) * 1000000LL;
}

template <typename State>
void StartArmEndpoints(State* state) {
  if (state->joint_names.empty()) {
    state->joint_names.reserve(6);
    for (int index = 1; index <= 6; ++index) {
      state->joint_names.push_back("joint_" + std::to_string(index));
    }
  }
  state->joint_states_sub = state->node->template create_subscription<sensor_msgs::msg::JointState>(
      state->config.joint_states.name, 10,
      [state](const sensor_msgs::msg::JointState::SharedPtr message) {
        if (!FreshHeader(state, message->header)) {
          eos::lib::LogOnce(eos::lib::GetLogger("ros2"), spdlog::level::warn,
                            state->device_id + ":joint_stale", "ros2_joint_telemetry_rejected",
                            {{"device", state->device_id}, {"reason", "stale_or_missing_header"}});
          return;
        }
        hardware::ManipulatorTelemetry next;
        {
          std::lock_guard<std::mutex> lock(state->state_mutex);
          if (state->manipulator_telemetry.has_value()) next = *state->manipulator_telemetry;
        }
        next.joint_valid.fill(false);
        next.joint_velocity_valid.fill(false);
        bool valid_sample = false;
        for (std::size_t target = 0; target < state->joint_names.size(); ++target) {
          std::size_t source = target;
          if (!message->name.empty()) {
            const auto found =
                std::find(message->name.begin(), message->name.end(), state->joint_names[target]);
            if (found == message->name.end()) continue;
            source = static_cast<std::size_t>(std::distance(message->name.begin(), found));
          }
          if (source >= message->position.size() || !std::isfinite(message->position[source])) {
            continue;
          }
          next.joint_angles_rad[target] = message->position[source];
          next.joint_valid[target] = true;
          valid_sample = true;
          if (source < message->velocity.size() && std::isfinite(message->velocity[source])) {
            next.joint_velocities_rad_s[target] = message->velocity[source];
            next.joint_velocity_valid[target] = true;
          }
        }
        if (!valid_sample || !std::all_of(next.joint_valid.begin(),
                                          next.joint_valid.begin() + state->joint_names.size(),
                                          [](bool valid) { return valid; })) {
          eos::lib::LogOnce(
              eos::lib::GetLogger("ros2"), spdlog::level::warn, state->device_id + ":joint_invalid",
              "ros2_joint_telemetry_rejected",
              {{"device", state->device_id}, {"reason", "missing_or_invalid_configured_joint"}});
          return;
        }
        std::lock_guard<std::mutex> lock(state->state_mutex);
        if (state->manipulator_telemetry.has_value() && state->manipulator_telemetry->pose_valid) {
          next.position_m[0] = state->manipulator_telemetry->position_m[0];
          next.position_m[1] = state->manipulator_telemetry->position_m[1];
          next.position_m[2] = state->manipulator_telemetry->position_m[2];
          std::copy(std::begin(state->manipulator_telemetry->orientation_quat),
                    std::end(state->manipulator_telemetry->orientation_quat),
                    std::begin(next.orientation_quat));
          next.pose_valid = true;
        }
        state->manipulator_telemetry = next;
        state->last_telemetry = std::chrono::steady_clock::now();
        state->last_joint = state->last_telemetry;
        eos::lib::LogOnce(eos::lib::GetLogger("ros2"), spdlog::level::info,
                          state->device_id + ":joint_received", "ros2_joint_telemetry_received",
                          {{"device", state->device_id},
                           {"topic", state->config.joint_states.name},
                           {"joints", state->joint_names.size()}});
      });
  if (state->kind == hardware::DeviceKind::kManipulator) {
    state->pose_query_sub =
        state->node->template create_subscription<geometry_msgs::msg::PoseStamped>(
            state->config.pose_query.name, 10,
            [state](const geometry_msgs::msg::PoseStamped::SharedPtr message) {
              if (!FreshHeader(state, message->header)) {
                eos::lib::LogOnce(
                    eos::lib::GetLogger("ros2"), spdlog::level::warn,
                    state->device_id + ":pose_stale", "ros2_pose_telemetry_rejected",
                    {{"device", state->device_id}, {"reason", "stale_or_missing_header"}});
                return;
              }
              const double position[3] = {message->pose.position.x, message->pose.position.y,
                                          message->pose.position.z};
              const double orientation[4] = {
                  message->pose.orientation.x, message->pose.orientation.y,
                  message->pose.orientation.z, message->pose.orientation.w};
              if (message->header.frame_id != state->config.base_frame ||
                  !std::isfinite(position[0]) || !std::isfinite(position[1]) ||
                  !std::isfinite(position[2]) || !ValidQuaternion(orientation)) {
                eos::lib::LogOnce(eos::lib::GetLogger("ros2"), spdlog::level::warn,
                                  state->device_id + ":pose_invalid",
                                  "ros2_pose_telemetry_rejected",
                                  {{"device", state->device_id},
                                   {"reason", "invalid_pose_or_unexpected_frame"},
                                   {"expected_frame", state->config.base_frame},
                                   {"received_frame", message->header.frame_id}});
                return;
              }
              std::lock_guard<std::mutex> lock(state->state_mutex);
              if (!state->manipulator_telemetry.has_value()) {
                state->manipulator_telemetry.emplace();
              }
              std::copy(std::begin(position), std::end(position),
                        std::begin(state->manipulator_telemetry->position_m));
              std::copy(std::begin(orientation), std::end(orientation),
                        std::begin(state->manipulator_telemetry->orientation_quat));
              state->manipulator_telemetry->pose_valid = true;
              state->last_telemetry = std::chrono::steady_clock::now();
              state->last_pose = state->last_telemetry;
              eos::lib::LogOnce(eos::lib::GetLogger("ros2"), spdlog::level::info,
                                state->device_id + ":pose_received", "ros2_pose_telemetry_received",
                                {{"device", state->device_id},
                                 {"topic", state->config.pose_query.name},
                                 {"frame", state->config.base_frame}});
            });
    if (!state->config.arm_status.name.empty()) {
      state->arm_status_sub = state->node->template create_subscription<std_msgs::msg::String>(
          state->config.arm_status.name, 10,
          [state](const std_msgs::msg::String::SharedPtr message) {
            constexpr std::size_t kMaximumStatusBytes = 64U * 1024U;
            try {
              if (message->data.empty() || message->data.size() > kMaximumStatusBytes) {
                throw std::invalid_argument("status payload size is invalid");
              }
              const nlohmann::json payload = nlohmann::json::parse(message->data);
              if (!payload.is_object() || payload.value("schema_version", "") != "1.0") {
                throw std::invalid_argument("unsupported status schema");
              }
              const auto& observed = payload.at("observed_at");
              if (!observed.is_object() || !observed.at("sec").is_number_integer() ||
                  !observed.at("nanosec").is_number_integer()) {
                throw std::invalid_argument("invalid status timestamp");
              }
              const std::int64_t seconds = observed.at("sec").get<std::int64_t>();
              const std::int64_t nanoseconds = observed.at("nanosec").get<std::int64_t>();
              if (seconds < 0 || nanoseconds < 0 || nanoseconds >= 1000000000LL ||
                  seconds >
                      (std::numeric_limits<std::int64_t>::max() - nanoseconds) / 1000000000LL) {
                throw std::invalid_argument("invalid status timestamp range");
              }
              const auto& controller = payload.at("controller");
              const auto& joints = payload.at("joints");
              if (!controller.is_object() || !controller.at("state").is_string() ||
                  controller.at("state").get_ref<const std::string&>().empty() ||
                  !controller.at("errors").is_array() || !joints.is_array() ||
                  joints.size() != state->joint_names.size()) {
                throw std::invalid_argument("invalid status structure");
              }

              std::vector<std::int64_t> errors;
              if (controller.at("errors").size() > 64) {
                throw std::invalid_argument("too many controller errors");
              }
              for (const auto& error : controller.at("errors")) {
                if (!error.is_number_integer()) {
                  throw std::invalid_argument("invalid controller error");
                }
                errors.push_back(error.get<std::int64_t>());
              }
              std::optional<std::int64_t> raw_code;
              if (controller.contains("raw_code")) {
                if (!controller.at("raw_code").is_number_integer()) {
                  throw std::invalid_argument("invalid controller raw code");
                }
                raw_code = controller.at("raw_code").get<std::int64_t>();
              }

              std::array<hardware::ManipulatorJointDiagnostics, 6> diagnostics{};
              for (std::size_t index = 0; index < joints.size(); ++index) {
                const auto& joint = joints.at(index);
                if (!joint.is_object() || joint.value("name", "") != state->joint_names[index]) {
                  throw std::invalid_argument("status joint order does not match binding");
                }
                auto& output = diagnostics[index];
                if (joint.contains("enabled")) {
                  if (!joint.at("enabled").is_boolean()) {
                    throw std::invalid_argument("invalid joint enabled value");
                  }
                  output.enabled = joint.at("enabled").get<bool>();
                }
                if (joint.contains("error_code")) {
                  if (!joint.at("error_code").is_number_integer()) {
                    throw std::invalid_argument("invalid joint error code");
                  }
                  output.error_code = joint.at("error_code").get<std::int64_t>();
                }
                for (const char* key : {"motor_current_raw", "temperature_raw", "voltage_raw"}) {
                  if (!joint.contains(key)) continue;
                  if (!joint.at(key).is_number()) {
                    throw std::invalid_argument(std::string("invalid joint ") + key);
                  }
                  const double value = joint.at(key).get<double>();
                  if (!std::isfinite(value)) {
                    throw std::invalid_argument(std::string("non-finite joint ") + key);
                  }
                  if (std::string_view(key) == "motor_current_raw")
                    output.motor_current_raw = value;
                  if (std::string_view(key) == "temperature_raw") output.temperature_raw = value;
                  if (std::string_view(key) == "voltage_raw") output.voltage_raw = value;
                }
              }

              std::lock_guard<std::mutex> lock(state->state_mutex);
              if (!state->manipulator_telemetry.has_value()) {
                state->manipulator_telemetry.emplace();
              }
              auto& telemetry = *state->manipulator_telemetry;
              telemetry.status_valid = true;
              telemetry.status_source_timestamp_ns = seconds * 1000000000LL + nanoseconds;
              telemetry.controller_state = controller.at("state").get<std::string>();
              telemetry.controller_status_code = raw_code;
              telemetry.controller_error_codes = std::move(errors);
              telemetry.joint_diagnostics = std::move(diagnostics);
              state->last_arm_status = std::chrono::steady_clock::now();
            } catch (const std::exception& error) {
              eos::lib::LogOnce(eos::lib::GetLogger("ros2"), spdlog::level::warn,
                                state->device_id + ":arm_status_invalid",
                                "ros2_arm_status_rejected",
                                {{"device", state->device_id}, {"reason", error.what()}});
            }
          });
    }
  }
  state->stop_pub =
      state->node->template create_publisher<std_msgs::msg::Empty>(state->config.stop.name, 10);
  state->estop_pub =
      state->node->template create_publisher<std_msgs::msg::Empty>(state->config.estop.name, 10);
  if (!state->config.movel.name.empty()) {
    state->movel_pub = state->node->template create_publisher<std_msgs::msg::Float64MultiArray>(
        state->config.movel.name, 10);
  }
  if (!state->config.movej.name.empty()) {
    state->movej_pub = state->node->template create_publisher<std_msgs::msg::Float64MultiArray>(
        state->config.movej.name, 10);
  }
  if (!state->config.pick.name.empty()) {
    state->pick_pub =
        state->node->template create_publisher<std_msgs::msg::String>(state->config.pick.name, 10);
  }
}

template <typename State>
void StartGripperEndpoints(State* state) {
  state->gripper_state_sub =
      state->node->template create_subscription<std_msgs::msg::Float64MultiArray>(
          state->config.gripper_state.name, 10,
          [state](const std_msgs::msg::Float64MultiArray::SharedPtr message) {
            if (message->data.size() != 3 || !std::isfinite(message->data[0]) ||
                message->data[0] < 0.0 || message->data[0] > 100.0 ||
                !std::isfinite(message->data[1]) || message->data[1] < 0.0 ||
                message->data[1] > 3.0 || std::floor(message->data[1]) != message->data[1] ||
                !std::isfinite(message->data[2]) ||
                (message->data[2] < 0.0 && message->data[2] != -1.0)) {
              return;
            }
            hardware::GripperTelemetry next;
            next.position_pct = message->data[0];
            next.state =
                static_cast<hardware::GripperState>(static_cast<std::uint8_t>(message->data[1]));
            if (message->data[2] >= 0.0) next.force_n = message->data[2];
            std::lock_guard<std::mutex> lock(state->state_mutex);
            state->gripper_telemetry = std::move(next);
            state->last_telemetry = std::chrono::steady_clock::now();
          });
  if (!state->config.gripper_command.name.empty())
    state->gripper_command_pub =
        state->node->template create_publisher<std_msgs::msg::Float64MultiArray>(
            state->config.gripper_command.name, 10);
  state->estop_pub =
      state->node->template create_publisher<std_msgs::msg::Empty>(state->config.estop.name, 10);
}

template <typename State>
void StartCameraEndpoints(State* state) {
  const rclcpp::QoS qos = rclcpp::SensorDataQoS().keep_last(1);
  state->image_sub = state->node->template create_subscription<sensor_msgs::msg::CompressedImage>(
      state->config.image.name, qos,
      [state](const sensor_msgs::msg::CompressedImage::SharedPtr message) {
        if (!FreshHeader(state, message->header) || message->header.frame_id.empty()) return;
        if ((message->format.find("jpeg") == std::string::npos &&
             message->format.find("jpg") == std::string::npos) ||
            message->data.empty() || message->data.size() > state->config.max_image_bytes) {
          return;
        }
        hardware::CameraTelemetry next;
        next.frame_id = message->header.frame_id;
        next.source_timestamp_ns = rclcpp::Time(message->header.stamp).nanoseconds();
        next.source_clock = hardware::SourceClockKind::kRos;
        next.source_clock_id =
            state->node->get_clock()->get_clock_type() == RCL_ROS_TIME ? "ros" : "ros-system";
        next.jpeg = message->data;
        if (!JpegDimensions(next.jpeg, &next.width_px, &next.height_px)) return;
        std::lock_guard<std::mutex> lock(state->state_mutex);
        if (state->config.depth_image.name.empty()) {
          state->camera_telemetry = std::move(next);
          state->last_telemetry = std::chrono::steady_clock::now();
        } else {
          state->pending_color = std::move(next);
          PublishAlignedCameraPair(state);
        }
      });
  if (!state->config.depth_image.name.empty()) {
    state->depth_image_sub = state->node->template create_subscription<sensor_msgs::msg::Image>(
        state->config.depth_image.name, qos,
        [state](const sensor_msgs::msg::Image::SharedPtr message) {
          if (!FreshHeader(state, message->header) || message->header.frame_id.empty() ||
              message->data.empty() || message->data.size() > state->config.max_image_bytes) {
            return;
          }
          AlignedDepthSample next;
          next.width_px = message->width;
          next.height_px = message->height;
          next.frame_id = message->header.frame_id;
          next.source_timestamp_ns = rclcpp::Time(message->header.stamp).nanoseconds();
          next.pixels = CopyDepthPixels(*message);
          if (next.pixels.empty()) return;
          std::lock_guard<std::mutex> lock(state->state_mutex);
          state->pending_depth = std::move(next);
          PublishAlignedCameraPair(state);
        });
  }
}

}  // namespace

Ros2Backend::Ros2Backend(std::string device_id, runtime::hardware_runtime::Ros2DeviceConfig config,
                         hardware::DeviceKind kind, std::vector<std::string> joint_names) try
    : impl_(std::make_unique<Impl>(std::move(device_id), std::move(config), kind,
                                   std::move(joint_names))) {
  if (kind != hardware::DeviceKind::kManipulator && kind != hardware::DeviceKind::kGimbal &&
      kind != hardware::DeviceKind::kGripper && kind != hardware::DeviceKind::kCamera) {
    throw std::invalid_argument(
        "ROS 2 backend supports only arm, gimbal, gripper, and camera devices");
  }
  if (impl_->joint_names.size() > 6) {
    throw std::invalid_argument("at most six joints supported");
  }
  if (kind == hardware::DeviceKind::kGimbal) {
    if (impl_->joint_names.size() != 6 || !impl_->config.gimbal_joint_map.has_value()) {
      throw std::invalid_argument("ROS 2 gimbal requires six joints and a gimbal joint map");
    }
    const auto& mapping = *impl_->config.gimbal_joint_map;
    const auto pan =
        std::find(impl_->joint_names.begin(), impl_->joint_names.end(), mapping.pan_joint);
    const auto tilt =
        std::find(impl_->joint_names.begin(), impl_->joint_names.end(), mapping.tilt_joint);
    if (pan == impl_->joint_names.end() || tilt == impl_->joint_names.end() || pan == tilt) {
      throw std::invalid_argument("ROS 2 gimbal joint map does not match its joint names");
    }
    impl_->gimbal_pan_index =
        static_cast<std::size_t>(std::distance(impl_->joint_names.begin(), pan));
    impl_->gimbal_tilt_index =
        static_cast<std::size_t>(std::distance(impl_->joint_names.begin(), tilt));
    impl_->gimbal_tilt_direction = mapping.tilt_direction;
  }
  impl_->context = std::make_shared<rclcpp::Context>();
  rclcpp::InitOptions init_options;
  init_options.set_domain_id(static_cast<std::size_t>(impl_->config.domain_id));
  init_options.auto_initialize_logging(false);
  impl_->context->init(0, nullptr, init_options);
  rclcpp::NodeOptions node_options;
  node_options.context(impl_->context);
  impl_->node = std::make_shared<rclcpp::Node>(NodeName(impl_->device_id),
                                               impl_->config.node_namespace, node_options);
  rclcpp::ExecutorOptions executor_options;
  executor_options.context = impl_->context;
  impl_->executor = std::make_unique<rclcpp::executors::SingleThreadedExecutor>(executor_options);
  const auto logger = eos::lib::GetLogger("ros2");
  if (kind == hardware::DeviceKind::kManipulator || kind == hardware::DeviceKind::kGimbal) {
    StartArmEndpoints(impl_.get());
  } else if (kind == hardware::DeviceKind::kGripper) {
    StartGripperEndpoints(impl_.get());
  } else {
    StartCameraEndpoints(impl_.get());
  }
  if (!impl_->config.command_result.name.empty()) StartCommandEndpoints(impl_.get());
  impl_->executor->add_node(impl_->node);
  if (!impl_->config.health.name.empty()) {
    impl_->health_sub = impl_->node->create_subscription<std_msgs::msg::Bool>(
        impl_->config.health.name, 1,
        [state = impl_.get(), logger](std_msgs::msg::Bool::ConstSharedPtr msg) {
          std::lock_guard<std::mutex> lock(state->state_mutex);
          if (!state->health_seen || state->adapter_alive != msg->data) {
            eos::lib::LogEvent(logger, spdlog::level::info, "ros2_adapter_health_changed",
                               {{"device", state->device_id}, {"alive", msg->data}});
          }
          state->adapter_alive = msg->data;
          state->health_seen = true;
          state->last_health = std::chrono::steady_clock::now();
        });
  }
  impl_->executor_thread = std::thread([state = impl_.get(), logger]() {
    try {
      state->executor->spin();
    } catch (const std::exception& error) {
      eos::lib::LogEvent(logger, spdlog::level::err, "ros2_executor_failed",
                         {{"device", state->device_id}, {"reason", error.what()}});
      std::lock_guard<std::mutex> lock(state->state_mutex);
      state->executor_failed = true;
      state->command_changed.notify_all();
    } catch (...) {
      eos::lib::LogEvent(logger, spdlog::level::err, "ros2_executor_failed",
                         {{"device", state->device_id}, {"reason", "unknown_exception"}});
      std::lock_guard<std::mutex> lock(state->state_mutex);
      state->executor_failed = true;
      state->command_changed.notify_all();
    }
  });
  impl_->started = true;
  eos::lib::LogEvent(logger, spdlog::level::info, "ros2_bridge_endpoints_configured",
                     {{"device", impl_->device_id},
                      {"joint_states_topic", impl_->config.joint_states.name},
                      {"pose_topic", impl_->config.pose_query.name},
                      {"health_topic", impl_->config.health.name},
                      {"movej_topic", impl_->config.movej.name},
                      {"movel_topic", impl_->config.movel.name},
                      {"pick_topic", impl_->config.pick.name},
                      {"command_result_topic", impl_->config.command_result.name},
                      {"command_cancel_topic", impl_->config.command_cancel.name},
                      {"telemetry_timeout_ms", impl_->config.telemetry_timeout_ms}});
  eos::lib::LogEvent(logger, spdlog::level::info, "ros2_bridge_started",
                     {{"kind", KindName(kind)},
                      {"device", impl_->device_id},
                      {"namespace", impl_->config.node_namespace},
                      {"domain", impl_->config.domain_id}});
} catch (const std::exception& error) {
  eos::lib::LogEvent(eos::lib::GetLogger("ros2"), spdlog::level::err, "ros2_bridge_start_failed",
                     {{"reason", error.what()}});
  throw;
} catch (...) {
  eos::lib::LogEvent(eos::lib::GetLogger("ros2"), spdlog::level::err, "ros2_bridge_start_failed",
                     {{"reason", "unknown_exception"}});
  throw;
}

Ros2Backend::~Ros2Backend() = default;

hardware::BackendIdentity Ros2Backend::Identify() const {
  return hardware::BackendIdentity{
      .name = std::string("ros2-") + KindName(impl_->kind),
      .version = "1.0.0",
      .device_id = impl_->device_id,
      .kind = impl_->kind,
  };
}

hardware::FeedDeclarations Ros2Backend::DeclareFeeds() const {
  hardware::FeedDeclarations feeds;
  if (impl_->kind == hardware::DeviceKind::kManipulator ||
      impl_->kind == hardware::DeviceKind::kGimbal ||
      impl_->kind == hardware::DeviceKind::kGripper) {
    feeds.proprioception.max_rate_hz = 10.0;
  } else if (impl_->kind == hardware::DeviceKind::kCamera) {
    feeds.sensor.max_rate_hz = 30.0;
  }
  feeds.health.max_rate_hz = 10.0;
  return feeds;
}

hardware::ReleaseResult Ros2Backend::ExecuteMoveL(const hardware::MoveLCommand& command) {
  if (impl_->kind != hardware::DeviceKind::kManipulator || impl_->movel_pub == nullptr ||
      command.command_id.empty() || impl_->command_result_sub == nullptr ||
      !ValidQuaternion(command.orientation_quat_xyzw) || !ValidPositive(command.speed_m_s) ||
      !std::isfinite(command.position_m[0]) || !std::isfinite(command.position_m[1]) ||
      !std::isfinite(command.position_m[2])) {
    LogCommandRejected(impl_.get(), "arm.movel", "invalid_command");
    return hardware::ReleaseResult::kRejected;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const std::string readiness = ReadinessReason(impl_.get());
    const std::size_t subscribers = impl_->movel_pub->get_subscription_count();
    if (readiness != "ready") {
      LogCommandRejected(impl_.get(), "arm.movel", readiness.c_str(), subscribers);
      return hardware::ReleaseResult::kRejected;
    }
    if (subscribers == 0) {
      LogCommandRejected(impl_.get(), "arm.movel", "no_command_subscriber", subscribers);
      return hardware::ReleaseResult::kRejected;
    }
  }
  std_msgs::msg::Float64MultiArray message;
  message.data = {command.position_m[0],
                  command.position_m[1],
                  command.position_m[2],
                  command.orientation_quat_xyzw[0],
                  command.orientation_quat_xyzw[1],
                  command.orientation_quat_xyzw[2],
                  command.orientation_quat_xyzw[3],
                  command.speed_m_s.value_or(-1.0)};
  Correlate(&message, command.command_id, "arm.movel", command.stream);
  if (!command.stream) PrepareCommand(impl_.get(), command.command_id);
  impl_->movel_pub->publish(message);
  eos::lib::LogEvent(eos::lib::GetLogger("ros2"),
                     command.stream ? spdlog::level::debug : spdlog::level::info,
                     "ros2_command_published",
                     {{"device", impl_->device_id},
                      {"command", "arm.movel"},
                      {"topic", impl_->config.movel.name}});
  return command.stream ? hardware::ReleaseResult::kAccepted
                        : WaitForCommandResult(impl_.get(), command.command_id);
}

hardware::ReleaseResult Ros2Backend::ExecuteMoveJ(const hardware::MoveJCommand& command) {
  if (impl_->kind != hardware::DeviceKind::kManipulator || impl_->movej_pub == nullptr ||
      command.command_id.empty() || impl_->command_result_sub == nullptr ||
      command.joint_positions_rad.size() != impl_->joint_names.size() ||
      !ValidPositive(command.speed_rad_s)) {
    LogCommandRejected(impl_.get(), "arm.movej", "invalid_command");
    return hardware::ReleaseResult::kRejected;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const std::string readiness = ReadinessReason(impl_.get(), false);
    const std::size_t subscribers = impl_->movej_pub->get_subscription_count();
    if (readiness != "ready") {
      LogCommandRejected(impl_.get(), "arm.movej", readiness.c_str(), subscribers);
      return hardware::ReleaseResult::kRejected;
    }
    if (subscribers == 0) {
      LogCommandRejected(impl_.get(), "arm.movej", "no_command_subscriber", subscribers);
      return hardware::ReleaseResult::kRejected;
    }
  }
  std_msgs::msg::Float64MultiArray message;
  message.data.reserve(command.joint_positions_rad.size() + 1);
  for (double position : command.joint_positions_rad) {
    if (!std::isfinite(position)) {
      LogCommandRejected(impl_.get(), "arm.movej", "non_finite_joint_target");
      return hardware::ReleaseResult::kRejected;
    }
    message.data.push_back(position);
  }
  message.data.push_back(command.speed_rad_s.value_or(-1.0));
  Correlate(&message, command.command_id, "arm.movej", command.stream);
  if (!command.stream) PrepareCommand(impl_.get(), command.command_id);
  impl_->movej_pub->publish(message);
  eos::lib::LogEvent(eos::lib::GetLogger("ros2"),
                     command.stream ? spdlog::level::debug : spdlog::level::info,
                     "ros2_command_published",
                     {{"device", impl_->device_id},
                      {"command", "arm.movej"},
                      {"topic", impl_->config.movej.name}});
  return command.stream ? hardware::ReleaseResult::kAccepted
                        : WaitForCommandResult(impl_.get(), command.command_id);
}

hardware::ReleaseResult Ros2Backend::ExecutePick(const hardware::PickCommand& command) {
  if (impl_->kind != hardware::DeviceKind::kManipulator || impl_->pick_pub == nullptr ||
      command.command_id.empty() || impl_->command_result_sub == nullptr ||
      command.object_id.size() <= 4 || command.object_id.rfind("obj:", 0) != 0) {
    LogCommandRejected(impl_.get(), "arm.pick", "invalid_command");
    return hardware::ReleaseResult::kRejected;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const std::string readiness = ReadinessReason(impl_.get());
    const std::size_t subscribers = impl_->pick_pub->get_subscription_count();
    if (readiness != "ready") {
      LogCommandRejected(impl_.get(), "arm.pick", readiness.c_str(), subscribers);
      return hardware::ReleaseResult::kRejected;
    }
    if (subscribers == 0) {
      LogCommandRejected(impl_.get(), "arm.pick", "no_command_subscriber", subscribers);
      return hardware::ReleaseResult::kRejected;
    }
  }
  std_msgs::msg::String message;
  message.data =
      nlohmann::json{{"command_id", command.command_id}, {"object_id", command.object_id}}.dump();
  PrepareCommand(impl_.get(), command.command_id);
  impl_->pick_pub->publish(message);
  eos::lib::LogEvent(eos::lib::GetLogger("ros2"), spdlog::level::info, "ros2_command_published",
                     {{"device", impl_->device_id},
                      {"command", "arm.pick"},
                      {"topic", impl_->config.pick.name},
                      {"object_id", command.object_id}});
  return WaitForCommandResult(impl_.get(), command.command_id, impl_->config.pick_timeout_ms);
}

hardware::ReleaseResult Ros2Backend::ExecuteGimbal(const hardware::GimbalCommand& command) {
  if (impl_->kind != hardware::DeviceKind::kGimbal || impl_->movej_pub == nullptr ||
      command.command_id.empty() || impl_->command_result_sub == nullptr ||
      !std::isfinite(command.pan_rad) || !std::isfinite(command.tilt_rad) ||
      !ValidPositive(command.speed_rad_s)) {
    LogCommandRejected(impl_.get(), "gimbal.motion", "invalid_command");
    return hardware::ReleaseResult::kRejected;
  }
  std_msgs::msg::Float64MultiArray message;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const std::string readiness = ReadinessReason(impl_.get());
    const std::size_t subscribers = impl_->movej_pub->get_subscription_count();
    if (readiness != "ready" || !impl_->manipulator_telemetry.has_value()) {
      LogCommandRejected(impl_.get(), "gimbal.motion", readiness.c_str(), subscribers);
      return hardware::ReleaseResult::kRejected;
    }
    if (subscribers == 0) {
      LogCommandRejected(impl_.get(), "gimbal.motion", "no_command_subscriber", subscribers);
      return hardware::ReleaseResult::kRejected;
    }
    message.data.assign(impl_->manipulator_telemetry->joint_angles_rad.begin(),
                        impl_->manipulator_telemetry->joint_angles_rad.end());
    message.data[impl_->gimbal_pan_index] = command.pan_rad;
    message.data[impl_->gimbal_tilt_index] = impl_->gimbal_tilt_direction * command.tilt_rad;
  }
  message.data.push_back(command.speed_rad_s.value_or(-1.0));
  Correlate(&message, command.command_id, "gimbal.motion", command.stream);
  if (!command.stream) PrepareCommand(impl_.get(), command.command_id);
  impl_->movej_pub->publish(message);
  eos::lib::LogEvent(eos::lib::GetLogger("ros2"),
                     command.stream ? spdlog::level::debug : spdlog::level::info,
                     "ros2_command_published",
                     {{"device", impl_->device_id},
                      {"command", "gimbal.motion"},
                      {"command_id", command.command_id},
                      {"topic", impl_->config.movej.name}});
  return command.stream ? hardware::ReleaseResult::kAccepted
                        : WaitForCommandResult(impl_.get(), command.command_id);
}

hardware::ReleaseResult Ros2Backend::ExecuteGripper(const hardware::GripperCommand& command) {
  if (impl_->kind != hardware::DeviceKind::kGripper || impl_->gripper_command_pub == nullptr ||
      command.command_id.empty() || impl_->command_result_sub == nullptr ||
      !ValidPositive(command.force_n) ||
      (command.opening_pct && (!std::isfinite(*command.opening_pct) ||
                               (*command.opening_pct != 0.0 && *command.opening_pct != 100.0))) ||
      (command.speed_pct && (!std::isfinite(*command.speed_pct) ||
                             *command.speed_pct < 1.0 || *command.speed_pct > 100.0)) ||
      (command.action != hardware::GripperAction::kOpen &&
       command.action != hardware::GripperAction::kClose)) {
    return hardware::ReleaseResult::kRejected;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (!impl_->Ready() || impl_->gripper_command_pub->get_subscription_count() == 0)
      return hardware::ReleaseResult::kRejected;
  }
  std_msgs::msg::Float64MultiArray message;
  message.data = {command.action == hardware::GripperAction::kOpen ? 0.0 : 1.0,
                  command.force_n.value_or(-1.0)};
  if (command.opening_pct || command.speed_pct) {
    message.data.push_back(command.opening_pct.value_or(-1.0));
    message.data.push_back(command.speed_pct.value_or(-1.0));
  }
  Correlate(&message, command.command_id, "gripper.motion");
  PrepareCommand(impl_.get(), command.command_id);
  impl_->gripper_command_pub->publish(message);
  return WaitForCommandResult(impl_.get(), command.command_id);
}

std::string Ros2Backend::ReleaseFailureReason() const {
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (!impl_->last_command_reason.empty()) return impl_->last_command_reason;
  }
  if (impl_->kind == hardware::DeviceKind::kGimbal) {
    if (impl_->movej_pub == nullptr) return "gimbal MoveJ endpoint is unavailable";
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const std::string readiness = ReadinessReason(impl_.get());
    if (readiness != "ready") return "gimbal " + readiness;
    if (impl_->movej_pub->get_subscription_count() == 0) return "no gimbal MoveJ subscriber";
  }
  if (impl_->kind == hardware::DeviceKind::kGripper) {
    if (impl_->gripper_command_pub == nullptr) return "gripper command endpoint is unavailable";
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const std::string readiness = ReadinessReason(impl_.get());
    if (readiness != "ready") return "gripper " + readiness;
    if (impl_->gripper_command_pub->get_subscription_count() == 0)
      return "no gripper command subscriber";
  }
  return "ROS 2 backend refused the release";
}

std::optional<hardware::ManipulatorTelemetry> Ros2Backend::ReadManipulatorTelemetry() const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  auto result = impl_->manipulator_telemetry;
  if (result) {
    if (!impl_->Fresh(impl_->last_joint)) result->joint_valid.fill(false);
    if (!impl_->Fresh(impl_->last_pose)) result->pose_valid = false;
    if (!impl_->Fresh(impl_->last_arm_status)) result->status_valid = false;
    if (!result->status_valid && !result->pose_valid &&
        std::none_of(result->joint_valid.begin(), result->joint_valid.end(),
                     [](bool valid) { return valid; })) {
      return std::nullopt;
    }
  }
  return result;
}

std::optional<hardware::GimbalTelemetry> Ros2Backend::ReadGimbalTelemetry() const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  if (impl_->kind != hardware::DeviceKind::kGimbal || impl_->halted ||
      !impl_->Fresh(impl_->last_joint) || !impl_->manipulator_telemetry.has_value()) {
    return std::nullopt;
  }
  const auto& telemetry = *impl_->manipulator_telemetry;
  return hardware::GimbalTelemetry{
      .pan_rad = telemetry.joint_angles_rad[impl_->gimbal_pan_index],
      .tilt_rad =
          impl_->gimbal_tilt_direction * telemetry.joint_angles_rad[impl_->gimbal_tilt_index],
      .pan_rate_rad_s = telemetry.joint_velocities_rad_s[impl_->gimbal_pan_index],
      .tilt_rate_rad_s = impl_->gimbal_tilt_direction *
                         telemetry.joint_velocities_rad_s[impl_->gimbal_tilt_index]};
}

std::optional<hardware::GripperTelemetry> Ros2Backend::ReadGripperTelemetry() const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  return impl_->Fresh(impl_->last_telemetry) ? impl_->gripper_telemetry : std::nullopt;
}

std::optional<hardware::CameraTelemetry> Ros2Backend::ReadCameraTelemetry() const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  return !impl_->halted && impl_->Fresh(impl_->last_telemetry) ? impl_->camera_telemetry
                                                               : std::nullopt;
}

hardware::ReleaseResult Ros2Backend::StopMotion() {
  if ((impl_->kind != hardware::DeviceKind::kManipulator &&
       impl_->kind != hardware::DeviceKind::kGimbal) ||
      impl_->stop_pub == nullptr) {
    return hardware::ReleaseResult::kRejected;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const std::size_t subscribers = impl_->stop_pub->get_subscription_count();
    if (subscribers == 0) {
      LogCommandRejected(impl_.get(), "motion.stop", "no_command_subscriber", subscribers);
      return hardware::ReleaseResult::kRejected;
    }
  }
  impl_->stop_pub->publish(std_msgs::msg::Empty{});
  return hardware::ReleaseResult::kAccepted;
}

hardware::ReleaseResult Ros2Backend::CancelCommand(const std::string& command_id) {
  if (command_id.empty() || impl_->command_cancel_pub == nullptr) {
    return hardware::ReleaseResult::kRejected;
  }
  if (impl_->command_cancel_pub->get_subscription_count() == 0) {
    return hardware::ReleaseResult::kRejected;
  }
  std_msgs::msg::String message;
  message.data = command_id;
  impl_->command_cancel_pub->publish(message);
  return hardware::ReleaseResult::kAccepted;
}

void Ros2Backend::Clamp() {
  if (impl_->stop_pub != nullptr)
    impl_->stop_pub->publish(std_msgs::msg::Empty{});
  else if (impl_->kind == hardware::DeviceKind::kGripper && impl_->estop_pub)
    impl_->estop_pub->publish(std_msgs::msg::Empty{});
}

void Ros2Backend::Halt() {
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (impl_->halted) return;
    impl_->halted = true;
  }
  impl_->command_changed.notify_all();
  if (impl_->estop_pub != nullptr) impl_->estop_pub->publish(std_msgs::msg::Empty{});
  eos::lib::LogEvent(eos::lib::GetLogger("ros2"), spdlog::level::warn, "ros2_backend_halted",
                     {{"device", impl_->device_id}});
}

hardware::BackendHealth Ros2Backend::ReadHealth() const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  return {.alive = impl_->Ready(), .admitted = IsAdmitted()};
}

}  // namespace eos::backends::ros2
