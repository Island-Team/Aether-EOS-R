#include "server.h"

#include <chrono>
#include <thread>

#include <sys/stat.h>

#include "src/eos/ext/simulator/world.h"
#include "src/eos/lib/log.h"

namespace eos::ext::simulator::ipc {
namespace wire = ::eos::simulator::v1;

namespace {
template <std::size_t N>
void CopyRepeated(const double (&source)[N], google::protobuf::RepeatedField<double>* target) {
  target->Reserve(static_cast<int>(N));
  for (double value : source) target->Add(value);
}

void FillBase(const BaseState& state, wire::BaseTelemetry* output) {
  output->set_x_m(state.x);
  output->set_y_m(state.y);
  output->set_yaw_rad(state.yaw);
  output->set_linear_x_m_s(state.vx);
  output->set_linear_y_m_s(state.vy);
  output->set_angular_z_rad_s(state.yaw_rate);
  output->set_posture(state.posture);
}

void FillGimbal(const GimbalState& state, wire::GimbalTelemetry* output) {
  output->set_pan_rad(state.pan_rad);
  output->set_tilt_rad(state.tilt_rad);
  output->set_pan_rate(state.pan_rate);
  output->set_tilt_rate(state.tilt_rate);
}
}  // namespace

bool SimulatorService::DeviceAvailable(const std::string& id) const {
  std::lock_guard lock(halted_mutex_);
  return world_.available() && halted_devices_.find(id) == halted_devices_.end();
}

grpc::Status SimulatorService::Health(grpc::ServerContext*, const wire::HealthRequest* request,
                                      wire::HealthResponse* response) {
  response->set_available(DeviceAvailable(request->device_id()));
  return grpc::Status::OK;
}

grpc::Status SimulatorService::Execute(grpc::ServerContext*, const wire::ExecuteRequest* request,
                                       wire::ExecuteResponse* response) {
  if (request->device_id().empty())
    return {grpc::StatusCode::INVALID_ARGUMENT, "device_id is required"};
  if (request->command_case() == wire::ExecuteRequest::kHalt) {
    std::lock_guard lock(halted_mutex_);
    const bool inserted = halted_devices_.insert(request->device_id()).second;
    response->set_accepted(true);
    if (inserted)
      lib::LogEvent(lib::GetLogger("simulator.ipc"), spdlog::level::warn,
                    "simulator_device_halted", {{"device_id", request->device_id()}});
    return grpc::Status::OK;
  }
  if (!DeviceAvailable(request->device_id())) {
    response->set_error_code("device_unavailable");
    return grpc::Status::OK;
  }
  switch (request->command_case()) {
    case wire::ExecuteRequest::kArmMoveL: {
      const auto& command = request->arm_move_l();
      if (command.position_m_size() != 3 || command.orientation_xyzw_size() != 4)
        return {grpc::StatusCode::INVALID_ARGUMENT, "arm pose dimensions are invalid"};
      double position[3], orientation[4], velocity[3]{};
      for (int i = 0; i < 3; ++i) position[i] = command.position_m(i);
      for (int i = 0; i < 4; ++i) orientation[i] = command.orientation_xyzw(i);
      world_.SetCartesian(position, orientation, velocity);
      break;
    }
    case wire::ExecuteRequest::kArmMoveJ:
      if (request->arm_move_j().positions_rad_size() != 6)
        return {grpc::StatusCode::INVALID_ARGUMENT, "arm joint dimensions are invalid"};
      for (int i = 0; i < 6; ++i)
        world_.SetJointPosition(static_cast<std::size_t>(i),
                                request->arm_move_j().positions_rad(i));
      break;
    case wire::ExecuteRequest::kArmStop:
      world_.StopArm();
      break;
    case wire::ExecuteRequest::kBaseDrive:
      world_.DriveBase(request->base_drive().linear_x_m_s(),
                       request->base_drive().linear_y_m_s(),
                       request->base_drive().angular_z_rad_s());
      break;
    case wire::ExecuteRequest::kBaseNavigateRelative:
      if (!world_.NavigateBaseRelativeChecked(
              request->base_navigate_relative().x_m(),
              request->base_navigate_relative().y_m(),
              request->base_navigate_relative().yaw_rad(),
              request->base_navigate_relative().footprint_width_m(),
              request->base_navigate_relative().footprint_length_m())) {
        response->set_error_code("collision");
        return grpc::Status::OK;
      }
      break;
    case wire::ExecuteRequest::kBasePosture:
      world_.SetBasePosture(static_cast<std::uint8_t>(request->base_posture().posture()));
      break;
    case wire::ExecuteRequest::kBaseStop:
      world_.StopBase();
      break;
    case wire::ExecuteRequest::kGimbalPoint:
      world_.PointGimbal(request->gimbal_point().pan_rad(), request->gimbal_point().tilt_rad(),
                         request->gimbal_point().pan_rate(), request->gimbal_point().tilt_rate());
      break;
    case wire::ExecuteRequest::kGimbalStop:
      world_.StopGimbal();
      break;
    case wire::ExecuteRequest::kGripperMove:
      world_.MoveGripper(request->gripper_move().close(), request->gripper_move().force_n());
      break;
    case wire::ExecuteRequest::kGripperArmGrasp:
      world_.ArmGrasp(request->gripper_arm_grasp().object_id());
      break;
    case wire::ExecuteRequest::kGripperStop:
      world_.StopGripper();
      break;
    case wire::ExecuteRequest::kSpeakerPlay:
      world_.PlayAudio(static_cast<std::size_t>(request->speaker_play().encoded_bytes()));
      break;
    case wire::ExecuteRequest::kSpeakerStop:
      world_.StopSpeaker();
      break;
    case wire::ExecuteRequest::COMMAND_NOT_SET:
    case wire::ExecuteRequest::kHalt:
      return {grpc::StatusCode::INVALID_ARGUMENT, "command is required"};
  }
  response->set_accepted(true);
  return grpc::Status::OK;
}

grpc::Status SimulatorService::ReadTelemetry(grpc::ServerContext*,
                                             const wire::ReadTelemetryRequest* request,
                                             wire::ReadTelemetryResponse* response) {
  if (!DeviceAvailable(request->device_id())) return grpc::Status::OK;
  switch (request->kind()) {
    case wire::ARM: {
      const ArmState state = world_.arm();
      auto* output = response->mutable_arm();
      CopyRepeated(state.ee_position, output->mutable_ee_position());
      CopyRepeated(state.ee_orientation_quat, output->mutable_ee_orientation_xyzw());
      CopyRepeated(state.ee_linear_velocity, output->mutable_ee_linear_velocity());
      CopyRepeated(state.joint_positions_rad, output->mutable_joint_positions_rad());
      CopyRepeated(state.joint_velocities, output->mutable_joint_velocities_rad_s());
      output->set_contact(state.contact);
      break;
    }
    case wire::BASE:
      FillBase(world_.base(), response->mutable_base());
      break;
    case wire::GIMBAL:
      FillGimbal(world_.gimbal(), response->mutable_gimbal());
      break;
    case wire::GRIPPER: {
      const GripperState state = world_.gripper();
      auto* output = response->mutable_gripper();
      output->set_jaw(static_cast<std::uint32_t>(state.state));
      output->set_object_held_id(state.object_held_id);
      output->set_force_n(state.force_n);
      output->set_position_pct(state.position_pct);
      break;
    }
    case wire::MICROPHONE: {
      const MicrophoneBlock state = world_.microphone();
      auto* output = response->mutable_microphone();
      output->set_sample_rate_hz(state.sample_rate_hz);
      output->set_channels(state.channels);
      for (std::int16_t sample : state.samples) output->add_samples(sample);
      break;
    }
    case wire::SPEAKER: {
      const SpeakerState state = world_.speaker();
      auto* output = response->mutable_speaker();
      output->set_enabled(state.enabled);
      output->set_device_present(state.device_present);
      output->set_playing(state.playing);
      output->set_remaining_s(state.remaining_s);
      break;
    }
    case wire::TELEMETRY_KIND_UNSPECIFIED:
      return {grpc::StatusCode::INVALID_ARGUMENT, "telemetry kind is required"};
  }
  response->set_available(true);
  return grpc::Status::OK;
}

grpc::Status SimulatorService::ReadCameraFrame(grpc::ServerContext*,
                                               const wire::ReadCameraFrameRequest* request,
                                               wire::ReadCameraFrameResponse* response) {
  if (!DeviceAvailable(request->device_id())) return grpc::Status::OK;
  const auto frame = world_.ReadCameraSource();
  if (!frame) return grpc::Status::OK;
  response->set_available(true);
  response->set_width_px(frame->width_px);
  response->set_height_px(frame->height_px);
  response->set_source_timestamp_ns(frame->source_timestamp_ns);
  response->set_jpeg(frame->jpeg.data(), frame->jpeg.size());
  auto* state = response->mutable_state();
  state->set_sequence(frame->state.state_sequence);
  FillBase(frame->state.base, state->mutable_base());
  FillGimbal(frame->state.gimbal, state->mutable_gimbal());
  state->set_scene_revision(frame->state.scene_revision);
  if (frame->state.selected_line_id) state->set_selected_line_id(*frame->state.selected_line_id);
  if (frame->state.selected_source_id)
    state->set_selected_source_id(*frame->state.selected_source_id);
  return grpc::Status::OK;
}

void SimulatorService::Serve(const std::filesystem::path& socket_path,
                             const std::atomic_bool& stop) {
  std::error_code ignored;
  std::filesystem::create_directories(socket_path.parent_path());
  std::filesystem::remove(socket_path, ignored);
  grpc::ServerBuilder builder;
  builder.SetMaxReceiveMessageSize(9 * 1024 * 1024);
  builder.SetMaxSendMessageSize(9 * 1024 * 1024);
  builder.AddListeningPort("unix:" + socket_path.string(), grpc::InsecureServerCredentials());
  builder.RegisterService(this);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server) throw std::runtime_error("failed to start simulator IPC server");
  if (::chmod(socket_path.c_str(), S_IRUSR | S_IWUSR) != 0) {
    server->Shutdown();
    throw std::runtime_error("failed to set simulator socket permissions");
  }
  auto logger = lib::GetLogger("simulator.ipc");
  lib::LogEvent(logger, spdlog::level::info, "simulator_ipc_started",
                {{"socket", socket_path.string()}});
  while (!stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  server->Shutdown();
  std::filesystem::remove(socket_path, ignored);
  lib::LogEvent(logger, spdlog::level::info, "simulator_ipc_stopped");
}

}  // namespace eos::ext::simulator::ipc
