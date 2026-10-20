#include "monitor.h"

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "simulator.h"

namespace eos::ext::simulator {
namespace {

nlohmann::json NumberArray(const double* values, std::size_t size) {
  nlohmann::json result = nlohmann::json::array();
  for (std::size_t index = 0; index < size; ++index) result.push_back(values[index]);
  return result;
}

const char* GripperStateName(GripperJaw state) {
  switch (state) {
    case GripperJaw::kOpen:
      return "open";
    case GripperJaw::kClosed:
      return "closed";
    case GripperJaw::kHolding:
      return "holding";
  }
  return "open";
}

nlohmann::json BuildSimulatorJson(const SimulatorSnapshot& snapshot) {
  const auto& map = snapshot.map;
  const auto& base = snapshot.base;
  const auto& gimbal = snapshot.gimbal;
  const auto& arm = snapshot.arm;
  const auto& gripper = snapshot.gripper;
  const auto& speaker = snapshot.speaker;
  nlohmann::json hit = nullptr;
  if (snapshot.camera_hit) {
    hit = {{"x_m", snapshot.camera_hit->point.x_m},
           {"y_m", snapshot.camera_hit->point.y_m},
           {"distance_m", snapshot.camera_hit->distance_m}};
  }
  const auto selected =
      snapshot.camera_hit
          ? std::pair<nlohmann::json, nlohmann::json>{snapshot.camera_hit->line_id,
                                                      snapshot.camera_hit->source_id}
          : std::pair<nlohmann::json, nlohmann::json>{nullptr, nullptr};
  nlohmann::json scene = SceneJson(snapshot.scene);
  scene.erase("version");
  return {
      {"type", "simulator.snapshot"},
      {"version", 2},
      {"state_sequence", snapshot.state_sequence},
      {"map",
       {{"id", map.id},
        {"revision", map.revision},
        {"raster",
         {{"width_px", map.width_px},
          {"height_px", map.height_px},
          {"meters_per_pixel", map.meters_per_pixel},
          {"origin", {{"x_m", map.origin_x_m}, {"y_m", map.origin_y_m}}}}},
        {"initial_base_pose",
         {{"x_m", map.initial_base_pose.x_m},
          {"y_m", map.initial_base_pose.y_m},
          {"yaw_rad", map.initial_base_pose.yaw_rad}}}}},
      {"scene", std::move(scene)},
      {"base",
       {{"pose", {{"x_m", base.x}, {"y_m", base.y}, {"yaw_rad", base.yaw}}},
        {"velocity",
         {{"linear_x_m_s", base.vx},
          {"linear_y_m_s", base.vy},
          {"angular_z_rad_s", base.yaw_rate}}},
        {"posture", base.posture}}},
      {"gimbal",
       {{"pan_rad", gimbal.pan_rad},
        {"tilt_rad", gimbal.tilt_rad},
        {"pan_rate_rad_s", gimbal.pan_rate},
        {"tilt_rate_rad_s", gimbal.tilt_rate}}},
      {"camera",
       {{"ray_heading_rad", base.yaw + gimbal.pan_rad},
        {"max_range_m", snapshot.camera_max_range_m},
        {"selected_line_id", selected.first},
        {"selected_source_id", selected.second},
        {"hit", std::move(hit)}}},
      {"arm",
       {{"ee_position_m", NumberArray(arm.ee_position, 3)},
        {"ee_orientation_quat", NumberArray(arm.ee_orientation_quat, 4)},
        {"ee_linear_velocity_m_s", NumberArray(arm.ee_linear_velocity, 3)},
        {"joint_positions_rad", NumberArray(arm.joint_positions_rad, 6)},
        {"joint_velocities_rad_s", NumberArray(arm.joint_velocities, 6)},
        {"contact", arm.contact}}},
      {"gripper",
       {{"state", GripperStateName(gripper.state)},
        {"object_held_id", gripper.object_held_id.empty() ? nlohmann::json(nullptr)
                                                          : nlohmann::json(gripper.object_held_id)},
        {"force_n", gripper.force_n},
        {"position_pct", gripper.position_pct}}},
      {"speaker",
       {{"enabled", speaker.enabled},
        {"device_present", speaker.device_present},
        {"playing", speaker.playing},
        {"remaining_s", speaker.remaining_s}}}};
}

}  // namespace

nlohmann::json SimulatorJson(const SimulatorSnapshot& snapshot) {
  return BuildSimulatorJson(snapshot);
}

}  // namespace eos::ext::simulator
