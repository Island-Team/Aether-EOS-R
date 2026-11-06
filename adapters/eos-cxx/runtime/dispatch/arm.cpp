#include <chrono>
#include <cmath>

#include "common.h"
#include "dispatcher.h"

namespace eos::runtime::dispatch {
using namespace detail;
namespace {
/// Multiplies XYZW quaternions in application order.
std::array<double, 4> MultiplyQuaternion(const double lhs[4], const double rhs[4]) {
  return {lhs[3] * rhs[0] + lhs[0] * rhs[3] + lhs[1] * rhs[2] - lhs[2] * rhs[1],
          lhs[3] * rhs[1] - lhs[0] * rhs[2] + lhs[1] * rhs[3] + lhs[2] * rhs[0],
          lhs[3] * rhs[2] + lhs[0] * rhs[1] - lhs[1] * rhs[0] + lhs[2] * rhs[3],
          lhs[3] * rhs[3] - lhs[0] * rhs[0] - lhs[1] * rhs[1] - lhs[2] * rhs[2]};
}

/// Rotates one XYZ vector by an XYZW unit quaternion.
std::array<double, 3> RotateVector(const double quaternion[4], const double vector[3]) {
  const double pure[4]{vector[0], vector[1], vector[2], 0.0};
  const double inverse[4]{-quaternion[0], -quaternion[1], -quaternion[2], quaternion[3]};
  const auto first = MultiplyQuaternion(quaternion, pure);
  const double first_raw[4]{first[0], first[1], first[2], first[3]};
  const auto rotated = MultiplyQuaternion(first_raw, inverse);
  return {rotated[0], rotated[1], rotated[2]};
}
}  // namespace

nlohmann::json Dispatcher::ArmMoveJ(const std::string& primitive, const std::string& version,
                                    hardware_runtime::Device& device,
                                    const nlohmann::json& arguments) {
  const hardware_runtime::PrimitiveDeclaration* declaration = DeclarationOf(device, primitive);
  const auto requested_reference = arguments.find("reference");
  if (declaration == nullptr || requested_reference == arguments.end() ||
      !requested_reference->is_string() ||
      !SupportsReference(*declaration, requested_reference->get<std::string>())) {
    return Ack(primitive, version, arguments, false, kUnsupportedReference,
               "joint-motion reference is not admitted for this device");
  }
  const auto positions = arguments.find("joint_positions_rad");
  const std::size_t dof = device.part().joint_names.empty() ? 6 : device.part().joint_names.size();
  if (positions == arguments.end() || !positions->is_array() || positions->size() != dof)
    return Ack(primitive, version, arguments, false, kInvalidArgument,
               "joint_positions_rad must match the device joint count");
  hardware::MoveJCommand command;
  command.command_id = arguments.value("command_id", "");
  command.stream = arguments.value("stream", false);
  command.joint_positions_rad.reserve(dof);
  std::optional<hardware::ManipulatorTelemetry> current;
  if (*requested_reference == "relative") {
    current = device.feeds().ReadManipulatorTelemetry();
    if (!current.has_value()) {
      return Ack(primitive, version, arguments, false, kInvalidState,
                 "relative joint motion requires fresh arm state");
    }
    for (std::size_t index = 0; index < dof; ++index) {
      if (!current->joint_valid[index]) {
        return Ack(primitive, version, arguments, false, kInvalidState,
                   "relative joint motion requires a complete arm state");
      }
    }
  }
  const hardware_runtime::NumericRange* joint_range =
      LimitOf(device, primitive, "joint_positions_rad");
  for (std::size_t index = 0; index < positions->size(); ++index) {
    const auto& position = (*positions)[index];
    if (!position.is_number())
      return Ack(primitive, version, arguments, false, kInvalidArgument,
                 "joint_positions_rad must contain finite numbers");
    double value = position.get<double>();
    if (current.has_value()) value += current->joint_angles_rad[index];
    if (joint_range == nullptr || !joint_range->Contains(value))
      return Ack(primitive, version, arguments, false, kInvalidArgument,
                 "joint_positions_rad is outside the admitted device range");
    command.joint_positions_rad.push_back(value);
  }
  if (!OptionalInAdmittedRange(arguments, "speed_rad_s", device, primitive, &command.speed_rad_s))
    return Ack(primitive, version, arguments, false, kInvalidArgument,
               "speed_rad_s is outside the admitted device range");
  command.stamp = std::chrono::steady_clock::now();
  return Released(primitive, version, arguments, device, device.backend().ExecuteMoveJ(command),
                  "arm refused the release");
}

nlohmann::json Dispatcher::ArmInit(const std::string& primitive, const std::string& version,
                                   hardware_runtime::Device& device,
                                   const nlohmann::json& arguments) {
  const auto& arm = device.part().arm;
  const std::size_t dof = device.part().joint_names.empty() ? 6 : device.part().joint_names.size();
  if (!arm.has_value() || arm->initial_joint_positions_rad.size() != dof)
    return Ack(primitive, version, arguments, false, kInvalidState,
               "arm has no valid configured initial joint pose");
  hardware::MoveJCommand command;
  command.command_id = arguments.value("command_id", "");
  command.joint_positions_rad = arm->initial_joint_positions_rad;
  command.stamp = std::chrono::steady_clock::now();
  return Released(primitive, version, arguments, device, device.backend().ExecuteMoveJ(command),
                  "arm refused the initial pose release");
}

nlohmann::json Dispatcher::ArmPick(const std::string& primitive, const std::string& version,
                                   hardware_runtime::Device& device,
                                   const nlohmann::json& arguments) {
  const auto object_id = arguments.find("object_id");
  if (object_id == arguments.end() || !object_id->is_string() ||
      object_id->get_ref<const std::string&>().rfind("obj:", 0) != 0) {
    return Ack(primitive, version, arguments, false, kInvalidArgument,
               "object_id must identify an obj:* world entity");
  }
  hardware::PickCommand command;
  command.command_id = arguments.value("command_id", "");
  command.object_id = object_id->get<std::string>();
  command.stamp = std::chrono::steady_clock::now();
  return Released(primitive, version, arguments, device, device.backend().ExecutePick(command),
                  "arm pick planner refused the object selection");
}

nlohmann::json Dispatcher::ArmMoveL(const std::string& primitive, const std::string& version,
                                    hardware_runtime::Device& device,
                                    const nlohmann::json& arguments) {
  const hardware_runtime::PrimitiveDeclaration* declaration = DeclarationOf(device, primitive);
  const auto requested_reference = arguments.find("reference");
  if (declaration == nullptr || requested_reference == arguments.end() ||
      !requested_reference->is_string() ||
      !SupportsReference(*declaration, requested_reference->get<std::string>())) {
    return Ack(primitive, version, arguments, false, kUnsupportedReference,
               "Cartesian-motion reference is not admitted for this device");
  }
  const auto position = arguments.find("position_m");
  const auto orientation = arguments.find("orientation_quat_xyzw");
  if (position == arguments.end() || orientation == arguments.end() || !position->is_array() ||
      !orientation->is_array() || position->size() != 3 || orientation->size() != 4)
    return Ack(primitive, version, arguments, false, kInvalidArgument,
               "a complete Cartesian pose is required");
  hardware::MoveLCommand command;
  command.command_id = arguments.value("command_id", "");
  command.stream = arguments.value("stream", false);
  for (std::size_t i = 0; i < 3; ++i) {
    if (!(*position)[i].is_number())
      return Ack(primitive, version, arguments, false, kInvalidArgument, "invalid position_m");
    command.position_m[i] = (*position)[i].get<double>();
    if (!std::isfinite(command.position_m[i]))
      return Ack(primitive, version, arguments, false, kInvalidArgument, "invalid position_m");
  }
  for (std::size_t i = 0; i < 4; ++i) {
    if (!(*orientation)[i].is_number())
      return Ack(primitive, version, arguments, false, kInvalidArgument,
                 "invalid orientation_quat_xyzw");
    command.orientation_quat_xyzw[i] = (*orientation)[i].get<double>();
    if (!std::isfinite(command.orientation_quat_xyzw[i]))
      return Ack(primitive, version, arguments, false, kInvalidArgument,
                 "invalid orientation_quat_xyzw");
  }
  double norm_squared = 0.0;
  for (double component : command.orientation_quat_xyzw) norm_squared += component * component;
  if (std::abs(norm_squared - 1.0) > 1e-6)
    return Ack(primitive, version, arguments, false, kInvalidArgument,
               "orientation_quat_xyzw must be unit-norm");
  if (*requested_reference == "absolute") {
    const auto frame = arguments.find("frame_id");
    if (!declaration->absolute_frame.has_value() || frame == arguments.end() ||
        !frame->is_string() || *frame != *declaration->absolute_frame) {
      return Ack(primitive, version, arguments, false, kUnsupportedReference,
                 "absolute Cartesian frame is not admitted for this device");
    }
  } else {
    const auto current = device.feeds().ReadManipulatorTelemetry();
    if (!current.has_value() || !current->pose_valid) {
      return Ack(primitive, version, arguments, false, kInvalidState,
                 "relative Cartesian motion requires fresh tool-pose state");
    }
    const auto offset = RotateVector(current->orientation_quat, command.position_m);
    for (std::size_t index = 0; index < 3; ++index) {
      command.position_m[index] = current->position_m[index] + offset[index];
    }
    const auto orientation =
        MultiplyQuaternion(current->orientation_quat, command.orientation_quat_xyzw);
    for (std::size_t index = 0; index < 4; ++index) {
      command.orientation_quat_xyzw[index] = orientation[index];
    }
  }
  if (!OptionalInAdmittedRange(arguments, "speed_m_s", device, primitive, &command.speed_m_s))
    return Ack(primitive, version, arguments, false, kInvalidArgument,
               "speed_m_s is outside the admitted device range");
  command.stamp = std::chrono::steady_clock::now();
  return Released(primitive, version, arguments, device, device.backend().ExecuteMoveL(command),
                  "arm refused the release");
}
}  // namespace eos::runtime::dispatch
