#include "ros2_backend.h"
#include "socket_protocol.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>

#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <cerrno>

#include "adapters/eos-cxx/ext/backends/ros2/descriptor.h"
#include "adapters/eos-cxx/runtime/hardware/manager/backend_catalog.h"
#include "adapters/eos-cxx/runtime/hardware/binding.h"
#include "adapters/eos-cxx/runtime/hardware/catalog.h"

namespace {
using Json = nlohmann::json;
using eos::backends::ros2::Ros2Backend;
using eos::backends::ros2::socket_protocol::Read;
using eos::backends::ros2::socket_protocol::Write;
using eos::hardware::Backend;
using eos::hardware::ReleaseResult;
volatile sig_atomic_t stopping = 0;
void Stop(int) { stopping = 1; }
template <typename T> Json Opt(const std::optional<T>& value) { return value ? Json(*value) : Json(nullptr); }
template <typename T> std::optional<T> GetOpt(const Json& value, const char* key) {
  if (!value.contains(key) || value[key].is_null()) return std::nullopt;
  return value[key].get<T>();
}
Json Gimbal(const eos::hardware::GimbalTelemetry& t) {
  return {{"pan", t.pan_rad}, {"tilt", t.tilt_rad}, {"pan_rate", t.pan_rate_rad_s},
          {"tilt_rate", t.tilt_rate_rad_s}};
}
Json Handle(Backend& backend, const Json& req) {
  if (req.value("v", 0) != eos::backends::ros2::socket_protocol::kVersion)
    throw std::runtime_error("unsupported bridge protocol version");
  const std::string op = req.at("op").get<std::string>();
  const Json args = req.value("args", Json::object());
  Json result = {{"v", 1}, {"ok", true}};
  auto release = [&](ReleaseResult value) { result["result"] = static_cast<int>(value); };
  if (op == "health") result["alive"] = backend.ReadHealth().alive;
  else if (op == "reason") result["reason"] = backend.ReleaseFailureReason();
  else if (op == "movej") {
    eos::hardware::MoveJCommand c; c.joint_positions_rad = args.at("positions").get<std::vector<double>>();
    c.speed_rad_s = GetOpt<double>(args,"speed"); c.command_id = args.at("command_id").get<std::string>();
    c.stream = args.value("stream", false); c.stamp = std::chrono::steady_clock::now(); release(backend.ExecuteMoveJ(c));
  } else if (op == "movel") {
    eos::hardware::MoveLCommand c;
    for (int i=0;i<3;++i) c.position_m[i]=args.at("position").at(i).get<double>();
    for (int i=0;i<4;++i) c.orientation_quat_xyzw[i]=args.at("orientation").at(i).get<double>();
    c.speed_m_s=GetOpt<double>(args,"speed"); c.command_id=args.at("command_id").get<std::string>();
    c.stream=args.value("stream",false); c.stamp=std::chrono::steady_clock::now(); release(backend.ExecuteMoveL(c));
  } else if (op == "pick") {
    eos::hardware::PickCommand c; c.object_id=args.at("object_id").get<std::string>();
    c.command_id=args.at("command_id").get<std::string>(); c.stamp=std::chrono::steady_clock::now(); release(backend.ExecutePick(c));
  } else if (op == "gimbal") {
    eos::hardware::GimbalCommand c; c.pan_rad=args.at("pan").get<double>(); c.tilt_rad=args.at("tilt").get<double>();
    c.speed_rad_s=GetOpt<double>(args,"speed"); c.command_id=args.at("command_id").get<std::string>();
    c.stream=args.value("stream",false); c.stamp=std::chrono::steady_clock::now(); release(backend.ExecuteGimbal(c));
  } else if (op == "gripper") {
    eos::hardware::GripperCommand c; c.action=static_cast<eos::hardware::GripperAction>(args.at("action").get<int>());
    c.force_n=GetOpt<double>(args,"force"); c.opening_pct=GetOpt<double>(args,"opening");
    c.speed_pct=GetOpt<double>(args,"speed"); c.command_id=args.at("command_id").get<std::string>(); release(backend.ExecuteGripper(c));
  } else if (op == "stop") release(backend.StopMotion());
  else if (op == "cancel") release(backend.CancelCommand(args.at("command_id").get<std::string>()));
  else if (op == "clamp") backend.Clamp();
  else if (op == "halt") backend.Halt();
  else if (op == "manipulator") {
    auto t=backend.ReadManipulatorTelemetry(); result["data"]=nullptr;
    if (t) {
      Json diagnostics=Json::array();
      for (const auto& d:t->joint_diagnostics) diagnostics.push_back({{"enabled",Opt(d.enabled)},{"error_code",Opt(d.error_code)},
        {"motor_current",Opt(d.motor_current_raw)},{"temperature",Opt(d.temperature_raw)},{"voltage",Opt(d.voltage_raw)}});
      result["data"]={{"angles",t->joint_angles_rad},{"velocities",t->joint_velocities_rad_s},
        {"joint_valid",t->joint_valid},{"velocity_valid",t->joint_velocity_valid},
        {"position",{t->position_m[0],t->position_m[1],t->position_m[2]}},
        {"orientation",{t->orientation_quat[0],t->orientation_quat[1],t->orientation_quat[2],t->orientation_quat[3]}},
        {"pose_valid",t->pose_valid},{"status_valid",t->status_valid},
        {"status_timestamp",Opt(t->status_source_timestamp_ns)},{"controller_state",t->controller_state},
        {"controller_status_code",Opt(t->controller_status_code)},{"controller_error_codes",t->controller_error_codes},
        {"joint_diagnostics",diagnostics}};
    }
  } else if (op == "gimbal_telemetry") {
    auto t=backend.ReadGimbalTelemetry(); result["data"]=t ? Gimbal(*t) : Json(nullptr);
  } else if (op == "gripper_telemetry") {
    auto t=backend.ReadGripperTelemetry(); result["data"]=t ? Json{{"state",static_cast<int>(t->state)},
      {"force",Opt(t->force_n)},{"position",Opt(t->position_pct)}} : Json(nullptr);
  } else if (op == "camera_telemetry") {
    auto t=backend.ReadCameraTelemetry(); result["data"]=nullptr;
    if (t) {
      Json intrinsics=nullptr;
      if (t->depth_intrinsics) { const auto& i=*t->depth_intrinsics;
        intrinsics={{"width",i.width_px},{"height",i.height_px},{"fx",i.fx},{"fy",i.fy},
          {"ppx",i.ppx},{"ppy",i.ppy},{"distortion_model",i.distortion_model},
          {"distortion_coefficients",i.distortion_coefficients}}; }
      result["data"]={{"width",t->width_px},{"height",t->height_px},{"frame_id",t->frame_id},
        {"source_timestamp",Opt(t->source_timestamp_ns)},{"source_clock",static_cast<int>(t->source_clock)},
        {"source_clock_id",t->source_clock_id},{"jpeg",t->jpeg},{"depth_png",t->depth_png},
        {"depth_u16",t->depth_u16},{"depth_scale",t->depth_scale},{"depth_intrinsics",intrinsics},
        {"capture_gimbal",t->capture_gimbal ? Gimbal(*t->capture_gimbal) : Json(nullptr)},
        {"capture_gimbal_sampled_monotonic_ns",Opt(t->capture_gimbal_sampled_monotonic_ns)}};
    }
  } else throw std::runtime_error("unknown bridge operation");
  return result;
}
int Main(int argc,char** argv) {
  std::string binding_path,profile,socket_path;
  for(int i=1;i<argc;++i) {
    std::string flag=argv[i];
    if(i+1>=argc) throw std::runtime_error("expected value for "+flag);
    if(flag=="--binding") binding_path=argv[++i];
    else if(flag=="--profile") profile=argv[++i];
    else if(flag=="--socket") socket_path=argv[++i];
    else throw std::runtime_error("unknown option: "+flag);
  }
  if(binding_path.empty()||profile.empty()||socket_path.empty()) throw std::runtime_error("usage: eos-ros2-bridge --binding PATH --profile NAME --socket PATH");
  if(socket_path.size()>=sizeof(sockaddr_un::sun_path)) throw std::runtime_error("socket path too long");
  auto catalog=eos::runtime::hardware_runtime::manager::BuildBackendCatalog();
  auto binding=eos::runtime::hardware_runtime::LoadBinding(binding_path,profile,catalog);
  std::map<std::string,std::unique_ptr<Backend>> devices;
  for(const auto& device:binding.devices) {
    if(device.backend_type!="ros2") continue;
    const auto& settings=static_cast<const eos::backends::Ros2Settings&>(*device.backend_settings);
    eos::hardware::DeviceKind kind=eos::hardware::DeviceKind::kCamera;
    if(device.part.kind=="arm") kind=eos::hardware::DeviceKind::kManipulator;
    else if(device.part.kind=="gimbal") kind=eos::hardware::DeviceKind::kGimbal;
    else if(device.part.kind=="gripper") kind=eos::hardware::DeviceKind::kGripper;
    std::unique_ptr<Backend> backend=std::make_unique<Ros2Backend>(device.device_id,settings.value,kind,device.part.joint_names);
    backend->MarkAdmitted();
    devices.emplace(device.device_id,std::move(backend));
  }
  if(devices.empty()) throw std::runtime_error("binding declares no ROS 2 devices");
  std::filesystem::create_directories(std::filesystem::path(socket_path).parent_path());
  const int server=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
  if(server<0) throw std::runtime_error(std::strerror(errno));
  struct Cleanup { int fd; std::string path; bool bound{false}; ~Cleanup(){close(fd);if(bound)unlink(path.c_str());} } cleanup{server,socket_path};
  sockaddr_un addr{}; addr.sun_family=AF_UNIX; std::memcpy(addr.sun_path,socket_path.c_str(),socket_path.size()+1);
  // Never unlink a possibly live or foreign socket. The manager is responsible
  // for verifying stale unit state before restarting this bridge.
  if(bind(server,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0) throw std::runtime_error(std::strerror(errno));
  cleanup.bound=true;
  chmod(socket_path.c_str(),0600);
  if(listen(server,16)<0) throw std::runtime_error(std::strerror(errno));
  struct sigaction action{}; action.sa_handler=Stop; sigemptyset(&action.sa_mask);
  sigaction(SIGTERM,&action,nullptr); sigaction(SIGINT,&action,nullptr);
  while(!stopping) {
    const int client=accept4(server,nullptr,nullptr,SOCK_CLOEXEC);
    if(client<0) { if(errno==EINTR) continue; throw std::runtime_error(std::strerror(errno)); }
    timeval timeout{40,0};
    setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    std::thread([client,&devices] {
      try {
        Json request=Read(client); const std::string id=request.at("device").get<std::string>();
        auto it=devices.find(id); if(it==devices.end()) throw std::runtime_error("unknown bridge device");
        Write(client,Handle(*it->second,request));
      } catch(const std::exception& error) {
        try { Write(client,{{"v",1},{"ok",false},{"error",error.what()}}); } catch(...) {}
      }
      close(client);
    }).detach();
  }
  return 0;
}
}
int main(int argc,char** argv) {
  try { return Main(argc,argv); }
  catch(const std::exception& error) { std::cerr << "eos-ros2-bridge: " << error.what() << '\n'; return 1; }
}
