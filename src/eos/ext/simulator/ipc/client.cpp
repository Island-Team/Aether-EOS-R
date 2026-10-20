#include "client.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <utility>

#include <grpcpp/grpcpp.h>
#include "simulator.grpc.pb.h"

namespace eos::ext::simulator::ipc {
namespace proto = ::eos::simulator::v1;

struct SimulatorClient::Impl {
  std::filesystem::path path;
  std::chrono::milliseconds deadline;
  std::unique_ptr<proto::SimulatorService::Stub> stub;

  Impl(std::filesystem::path socket_path, std::chrono::milliseconds timeout)
      : path(std::move(socket_path)), deadline(timeout) {
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(8 * 1024 * 1024);
    args.SetMaxSendMessageSize(256 * 1024);
    stub = proto::SimulatorService::NewStub(
        grpc::CreateCustomChannel("unix:" + path.string(), grpc::InsecureChannelCredentials(), args));
  }
  void Bound(grpc::ClientContext& context) const {
    context.set_deadline(std::chrono::system_clock::now() + deadline);
  }
  bool Execute(proto::ExecuteRequest request) const {
    grpc::ClientContext context; Bound(context);
    proto::ExecuteResponse response;
    return stub->Execute(&context, request, &response).ok() && response.accepted();
  }
  std::optional<proto::ReadTelemetryResponse> Telemetry(const std::string& id,
                                                        proto::TelemetryKind kind) const {
    grpc::ClientContext context; Bound(context);
    proto::ReadTelemetryRequest request; request.set_device_id(id); request.set_kind(kind);
    proto::ReadTelemetryResponse response;
    if (!stub->ReadTelemetry(&context, request, &response).ok() || !response.available()) return {};
    return response;
  }
};

SimulatorClient::SimulatorClient(std::filesystem::path path, std::chrono::milliseconds deadline)
    : impl_(std::make_unique<Impl>(
          path == "var/run/simulator.sock" && std::getenv("EOS_SIMULATOR_SOCKET") != nullptr
              ? std::filesystem::path(std::getenv("EOS_SIMULATOR_SOCKET"))
              : std::move(path),
          deadline)) {}
SimulatorClient::~SimulatorClient() = default;
const std::filesystem::path& SimulatorClient::socket_path() const { return impl_->path; }
bool SimulatorClient::Available(const std::string& id) const {
  grpc::ClientContext context; impl_->Bound(context);
  proto::HealthRequest request; request.set_device_id(id); proto::HealthResponse response;
  return impl_->stub->Health(&context, request, &response).ok() && response.available();
}

bool SimulatorClient::SetCartesian(const std::string& id, const double p[3], const double q[4]) {
  proto::ExecuteRequest r; r.set_device_id(id); auto* c=r.mutable_arm_move_l();
  for(int i=0;i<3;++i)c->add_position_m(p[i]); for(int i=0;i<4;++i)c->add_orientation_xyzw(q[i]);
  return impl_->Execute(std::move(r));
}
bool SimulatorClient::SetJointPositions(const std::string& id,const std::vector<double>& v){proto::ExecuteRequest r;r.set_device_id(id);auto*c=r.mutable_arm_move_j();for(double x:v)c->add_positions_rad(x);return impl_->Execute(std::move(r));}
bool SimulatorClient::StopArm(const std::string&id){proto::ExecuteRequest r;r.set_device_id(id);r.mutable_arm_stop();return impl_->Execute(std::move(r));}
std::optional<ArmState> SimulatorClient::ReadArm(const std::string&id)const{auto r=impl_->Telemetry(id,proto::ARM);if(!r||!r->has_arm())return{};ArmState s{};const auto&t=r->arm();for(int i=0;i<std::min(3,t.ee_position_size());++i)s.ee_position[i]=t.ee_position(i);for(int i=0;i<std::min(4,t.ee_orientation_xyzw_size());++i)s.ee_orientation_quat[i]=t.ee_orientation_xyzw(i);for(int i=0;i<std::min(3,t.ee_linear_velocity_size());++i)s.ee_linear_velocity[i]=t.ee_linear_velocity(i);for(int i=0;i<std::min(6,t.joint_positions_rad_size());++i)s.joint_positions_rad[i]=t.joint_positions_rad(i);for(int i=0;i<std::min(6,t.joint_velocities_rad_s_size());++i)s.joint_velocities[i]=t.joint_velocities_rad_s(i);s.contact=t.contact();return s;}
bool SimulatorClient::DriveBase(const std::string&id,double x,double y,double z){proto::ExecuteRequest r;r.set_device_id(id);auto*c=r.mutable_base_drive();c->set_linear_x_m_s(x);c->set_linear_y_m_s(y);c->set_angular_z_rad_s(z);return impl_->Execute(std::move(r));}
bool SimulatorClient::NavigateBaseRelative(const std::string& id, double x, double y, double yaw,
                                           double footprint_width,
                                           double footprint_length) {
  proto::ExecuteRequest request;
  request.set_device_id(id);
  auto* command = request.mutable_base_navigate_relative();
  command->set_x_m(x);
  command->set_y_m(y);
  command->set_yaw_rad(yaw);
  command->set_footprint_width_m(footprint_width);
  command->set_footprint_length_m(footprint_length);
  return impl_->Execute(std::move(request));
}
bool SimulatorClient::SetBasePosture(const std::string&id,std::uint8_t p){proto::ExecuteRequest r;r.set_device_id(id);r.mutable_base_posture()->set_posture(p);return impl_->Execute(std::move(r));}
bool SimulatorClient::StopBase(const std::string&id){proto::ExecuteRequest r;r.set_device_id(id);r.mutable_base_stop();return impl_->Execute(std::move(r));}
std::optional<BaseState> SimulatorClient::ReadBase(const std::string&id)const{auto r=impl_->Telemetry(id,proto::BASE);if(!r||!r->has_base())return{};const auto&t=r->base();return BaseState{t.x_m(),t.y_m(),t.yaw_rad(),t.linear_x_m_s(),t.linear_y_m_s(),t.angular_z_rad_s(),static_cast<std::uint8_t>(t.posture())};}
bool SimulatorClient::PointGimbal(const std::string&id,double p,double t,double pr,double tr){proto::ExecuteRequest r;r.set_device_id(id);auto*c=r.mutable_gimbal_point();c->set_pan_rad(p);c->set_tilt_rad(t);c->set_pan_rate(pr);c->set_tilt_rate(tr);return impl_->Execute(std::move(r));}
bool SimulatorClient::StopGimbal(const std::string&id){proto::ExecuteRequest r;r.set_device_id(id);r.mutable_gimbal_stop();return impl_->Execute(std::move(r));}
std::optional<GimbalState> SimulatorClient::ReadGimbal(const std::string&id)const{auto r=impl_->Telemetry(id,proto::GIMBAL);if(!r||!r->has_gimbal())return{};const auto&t=r->gimbal();return GimbalState{t.pan_rad(),t.tilt_rad(),t.pan_rate(),t.tilt_rate()};}
bool SimulatorClient::MoveGripper(const std::string&id,bool close,double force){proto::ExecuteRequest r;r.set_device_id(id);auto*c=r.mutable_gripper_move();c->set_close(close);c->set_force_n(force);return impl_->Execute(std::move(r));}
bool SimulatorClient::ArmGrasp(const std::string&id,const std::string&o){proto::ExecuteRequest r;r.set_device_id(id);r.mutable_gripper_arm_grasp()->set_object_id(o);return impl_->Execute(std::move(r));}
bool SimulatorClient::StopGripper(const std::string&id){proto::ExecuteRequest r;r.set_device_id(id);r.mutable_gripper_stop();return impl_->Execute(std::move(r));}
std::optional<GripperState> SimulatorClient::ReadGripper(const std::string&id)const{auto r=impl_->Telemetry(id,proto::GRIPPER);if(!r||!r->has_gripper())return{};const auto&t=r->gripper();return GripperState{static_cast<GripperJaw>(t.jaw()),t.object_held_id(),t.force_n(),t.position_pct()};}
bool SimulatorClient::PlayAudio(const std::string&id,std::size_t n){proto::ExecuteRequest r;r.set_device_id(id);r.mutable_speaker_play()->set_encoded_bytes(n);return impl_->Execute(std::move(r));}
bool SimulatorClient::StopSpeaker(const std::string&id){proto::ExecuteRequest r;r.set_device_id(id);r.mutable_speaker_stop();return impl_->Execute(std::move(r));}
std::optional<SpeakerState> SimulatorClient::ReadSpeaker(const std::string&id)const{auto r=impl_->Telemetry(id,proto::SPEAKER);if(!r||!r->has_speaker())return{};const auto&t=r->speaker();return SpeakerState{t.enabled(),t.device_present(),t.playing(),t.remaining_s()};}
std::optional<MicrophoneBlock> SimulatorClient::ReadMicrophone(const std::string&id)const{auto r=impl_->Telemetry(id,proto::MICROPHONE);if(!r||!r->has_microphone())return{};MicrophoneBlock b{};const auto&t=r->microphone();b.sample_rate_hz=t.sample_rate_hz();b.channels=static_cast<std::uint16_t>(t.channels());for(int i=0;i<std::min(64,t.samples_size());++i)b.samples[i]=static_cast<std::int16_t>(t.samples(i));return b;}
std::optional<CameraSourceFrame> SimulatorClient::ReadCameraSource(const std::string&id)const{grpc::ClientContext c;impl_->Bound(c);proto::ReadCameraFrameRequest q;q.set_device_id(id);proto::ReadCameraFrameResponse r;if(!impl_->stub->ReadCameraFrame(&c,q,&r).ok()||!r.available()||r.jpeg().size()>8*1024*1024)return{};CameraSourceFrame f;f.width_px=r.width_px();f.height_px=r.height_px();f.source_timestamp_ns=r.source_timestamp_ns();f.jpeg.assign(r.jpeg().begin(),r.jpeg().end());const auto&s=r.state();f.state.state_sequence=s.sequence();f.state.scene_revision=s.scene_revision();if(s.has_selected_line_id())f.state.selected_line_id=s.selected_line_id();if(s.has_selected_source_id())f.state.selected_source_id=s.selected_source_id();const auto&b=s.base();f.state.base={b.x_m(),b.y_m(),b.yaw_rad(),b.linear_x_m_s(),b.linear_y_m_s(),b.angular_z_rad_s(),static_cast<std::uint8_t>(b.posture())};const auto&g=s.gimbal();f.state.gimbal={g.pan_rad(),g.tilt_rad(),g.pan_rate(),g.tilt_rate()};return f;}
bool SimulatorClient::Halt(const std::string&id){proto::ExecuteRequest r;r.set_device_id(id);r.mutable_halt();return impl_->Execute(std::move(r));}

}  // namespace eos::ext::simulator::ipc
