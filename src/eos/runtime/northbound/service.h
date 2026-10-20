/// Transport-neutral gateway service and native gRPC adapter.
#pragma once

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "eos/hardware/backend.h"
#include "eos/gateway.grpc.pb.h"
#include "src/eos/runtime/models/runtime.h"
#include "src/eos/runtime/perception/producer_coordinator.h"
#include "src/eos/runtime/perception/product_history.h"

namespace eos::runtime::init {
class SchemaStore;
}
namespace eos::runtime::hardware_runtime {
class Registry;
class TelemetryHub;
}  // namespace eos::runtime::hardware_runtime
namespace eos::runtime::perception::body {
class Reader;
}
namespace eos::runtime::perception {
class ResourceCatalog;
}
namespace eos::runtime::models { class CameraPoseStore; class CameraObjectStore; }
namespace eos::runtime::dispatch {
class Dispatcher;
}
namespace eos::runtime::store {
class NavigationPointStore;
}
namespace eos::runtime::northbound {

struct NorthboundServices {
  perception::body::Reader* projection{nullptr};
  dispatch::Dispatcher* dispatcher{nullptr};
  const hardware_runtime::Registry* registry{nullptr};
  models::Runtime* models{nullptr};
  hardware_runtime::TelemetryHub* telemetry{nullptr};
  const perception::ResourceCatalog* resources{nullptr};
  store::NavigationPointStore* navigation_points{nullptr};
  const models::CameraPoseStore* camera_pose{nullptr};
  const models::CameraObjectStore* camera_object{nullptr};
  struct PerceptionRole { std::string role; std::string model_id; bool continuous{false}; };
  std::vector<PerceptionRole> perception_roles;
  std::string perception_camera_device_id;
  std::string object_model_id;
};

struct ResourceHistoryQuery {
  enum class Kind { kSequence, kBefore, kAfter, kNearest, kSequenceRange };
  Kind kind{Kind::kSequence};
  std::uint64_t sequence{0};
  std::uint64_t last_sequence{0};
  std::size_t limit{0};
  std::chrono::system_clock::time_point timestamp;
};

struct RpcResult {
  grpc::StatusCode code{grpc::StatusCode::OK};
  std::string message;
  nlohmann::json payload;
  std::optional<std::chrono::milliseconds> sample_age;

  bool ok() const { return code == grpc::StatusCode::OK; }
  static RpcResult Error(grpc::StatusCode code, std::string message);
};

struct MonitorCameraSample {
  std::uint32_t width_px{0};
  std::uint32_t height_px{0};
  std::uint64_t sequence{0};
  std::optional<std::int64_t> source_timestamp_ns;
  std::chrono::system_clock::time_point captured_at;
  std::vector<std::uint8_t> jpeg;
};

struct MonitorAudioSample {
  double rms{0.0};
  double peak{0.0};
  double dbfs{0.0};
  bool clipping{false};
  std::uint64_t sequence{0};
  std::optional<std::int64_t> source_timestamp_ns;
  std::chrono::system_clock::time_point captured_at;
  std::uint32_t sample_rate_hz{0};
  std::uint32_t channels{0};
};

struct MonitorSample {
  std::optional<MonitorCameraSample> camera;
  std::optional<MonitorAudioSample> audio;
};

MonitorAudioSample CalculateAudioEnergy(const hardware::MicrophoneTelemetry& telemetry,
                                        std::uint64_t sequence,
                                        std::chrono::system_clock::time_point captured_at);

/// Serializes projection state and the runtime tick. Action admission and
/// native skill releases are serialized by the transport-neutral Dispatcher.
class NorthboundCore final {
 public:
  NorthboundCore(NorthboundServices services, std::function<void()> tick);
  ~NorthboundCore();

  RpcResult Discover();
  RpcResult GetStatus(const std::string& correlation_id);
  RpcResult GetModelStatus(const std::string& model_id);
  RpcResult ListResources();
  RpcResult ReadResource(const std::string& uri);
  RpcResult ListNavigationPoints(const std::string& robot_name);
  RpcResult GetNavigationPoint(const std::string& robot_name, const std::string& name);
  RpcResult PutNavigationPoint(const std::string& robot_name, const std::string& name, double x_m,
                               double y_m, double yaw_rad);
  RpcResult DeleteNavigationPoint(const std::string& robot_name, const std::string& name);
  RpcResult ReloadNavigationPoints();
  RpcResult QueryResourceHistory(const std::string& uri, const ResourceHistoryQuery& query);
  std::optional<perception::ProductLookup> LatestPublishedResource(const std::string& uri) const;
  std::optional<perception::ProductWait> WaitForPublishedResource(
      const std::string& uri, std::uint64_t after_sequence,
      const std::function<bool()>& cancel_requested) const;
  std::optional<perception::ProducerLease> AcquireResourceDemand(const std::string& uri) const;
  /// Polling read shared by WatchResource streams. Snapshots for one URI are
  /// sampled at most once per watch interval, regardless of watcher count.
  RpcResult ReadResourceForWatch(const std::string& uri);
  RpcResult CallPrimitive(const std::string& name, const std::string& version,
                          const std::string& device_id, nlohmann::json arguments,
                          const std::string& command_id);
  RpcResult CancelPrimitive(const std::string& command_id);
  RpcResult CaptureFrames(const std::string& robot_name, const std::string& device_id,
                          std::uint32_t frame_count);
  RpcResult ObserveObjects(const std::string& robot_name, const std::string& device_id,
                           const std::string& model_id,
                           std::optional<std::int64_t> source_timestamp_ns = std::nullopt,
                           models::CancelRequested cancel_requested = {});
  RpcResult InferModel(const std::string& model_id, models::Capability capability,
                       nlohmann::json request, models::CancelRequested cancel_requested);
  RpcResult ValidateMonitor(const std::string& camera_device_id,
                            const std::string& microphone_device_id);
  MonitorSample ReadMonitor(const std::string& camera_device_id,
                            const std::string& microphone_device_id);
  void Tick();

 private:
  friend class NorthboundGrpcService;
  RpcResult BuildDiscover();
  RpcResult BuildStatus();
  RpcResult BuildResourceList();
  void RefreshPublicSnapshots();
  void RefreshProjectedSnapshots();
  bool DeviceAliveSnapshot(const std::string& device_id);
  RpcResult ReadResource(const std::string& uri, bool use_watch_cache);

  struct CachedResource {
    std::chrono::steady_clock::time_point sampled_at;
    nlohmann::json payload;
  };

  NorthboundServices services_;
  std::function<void()> tick_;
  std::mutex mutex_;
  std::map<std::string, CachedResource> watch_cache_;
  std::mutex body_mutex_;
  std::map<std::string, CachedResource> body_snapshots_;
  std::mutex public_snapshot_mutex_;
  std::optional<CachedResource> discovery_snapshot_;
  std::optional<CachedResource> status_snapshot_;
  std::optional<CachedResource> resource_list_snapshot_;
  struct CachedFrame {
    std::uint32_t width_px{0};
    std::uint32_t height_px{0};
    std::chrono::system_clock::time_point captured_at;
    std::optional<std::int64_t> source_timestamp_ns;
    std::vector<std::uint8_t> jpeg;
  };
  void SampleCameraFrames();
  std::map<std::string, std::deque<CachedFrame>> camera_frames_;
  std::map<std::string, std::uint64_t> monitor_camera_sequences_;
  std::map<std::string, std::uint64_t> monitor_audio_sequences_;
  std::chrono::steady_clock::time_point steady_start_;
  std::chrono::system_clock::time_point system_start_;
};

class NorthboundGrpcService final : public island::eos::v1::GatewayService::Service {
 public:
  explicit NorthboundGrpcService(NorthboundCore& core) : core_(core) {}

  grpc::Status Discover(grpc::ServerContext*, const google::protobuf::Empty*,
                        island::eos::v1::DiscoverResponse*) override;
  grpc::Status GetStatus(grpc::ServerContext*, const google::protobuf::Empty*,
                         google::protobuf::Struct*) override;
  grpc::Status GetModelStatus(grpc::ServerContext*, const island::eos::v1::ModelStatusRequest*,
                              island::eos::v1::ModelStatus*) override;
  grpc::Status WatchModelStatus(grpc::ServerContext*, const island::eos::v1::ModelStatusRequest*,
                                grpc::ServerWriter<island::eos::v1::ModelStatus>*) override;
  grpc::Status ListResources(grpc::ServerContext*, const google::protobuf::Empty*,
                             google::protobuf::ListValue*) override;
  grpc::Status ReadResource(grpc::ServerContext*, const island::eos::v1::ResourceRequest*,
                            google::protobuf::Struct*) override;
  grpc::Status ListNavigationPoints(grpc::ServerContext*,
                                    const island::eos::v1::ListNavigationPointsRequest*,
                                    island::eos::v1::ListNavigationPointsResponse*) override;
  grpc::Status GetNavigationPoint(grpc::ServerContext*,
                                  const island::eos::v1::GetNavigationPointRequest*,
                                  island::eos::v1::NavigationPoint*) override;
  grpc::Status PutNavigationPoint(grpc::ServerContext*,
                                  const island::eos::v1::PutNavigationPointRequest*,
                                  island::eos::v1::NavigationPoint*) override;
  grpc::Status DeleteNavigationPoint(grpc::ServerContext*,
                                     const island::eos::v1::GetNavigationPointRequest*,
                                     google::protobuf::Empty*) override;
  grpc::Status ReloadNavigationPoints(grpc::ServerContext*, const google::protobuf::Empty*,
                                      island::eos::v1::ReloadNavigationPointsResponse*) override;
  grpc::Status QueryResourceHistory(grpc::ServerContext*,
                                    const island::eos::v1::QueryResourceHistoryRequest*,
                                    island::eos::v1::QueryResourceHistoryResponse*) override;
  grpc::Status WatchResource(grpc::ServerContext*, const island::eos::v1::ResourceRequest*,
                             grpc::ServerWriter<island::eos::v1::ResourceChanged>*) override;
  grpc::Status CallPrimitive(grpc::ServerContext*, const island::eos::v1::CallPrimitiveRequest*,
                             google::protobuf::Struct*) override;
  grpc::Status CancelPrimitive(grpc::ServerContext*,
                               const island::eos::v1::PrimitiveCommandRequest*,
                               google::protobuf::Struct*) override;
  grpc::Status CaptureFrames(grpc::ServerContext*, const island::eos::v1::CaptureFramesRequest*,
                             island::eos::v1::CaptureFramesResponse*) override;
  grpc::Status ObserveObjects(grpc::ServerContext*, const island::eos::v1::ObserveObjectsRequest*,
                              island::eos::v1::ObserveObjectsResponse*) override;
  grpc::Status InferVad(grpc::ServerContext*, const island::eos::v1::InferVadRequest*,
                        island::eos::v1::InferVadResponse*) override;
  grpc::Status DetectHumans(grpc::ServerContext*, const island::eos::v1::DetectHumansRequest*,
                            island::eos::v1::DetectHumansResponse*) override;
  grpc::Status EstimateHumanPose(grpc::ServerContext*,
                                 const island::eos::v1::EstimateHumanPoseRequest*,
                                 island::eos::v1::EstimateHumanPoseResponse*) override;
  grpc::Status DetectFaces(grpc::ServerContext*, const island::eos::v1::DetectFacesRequest*,
                           island::eos::v1::DetectFacesResponse*) override;
  grpc::Status WatchMonitor(grpc::ServerContext*, const island::eos::v1::MonitorRequest*,
                            grpc::ServerWriter<island::eos::v1::MonitorEvent>*) override;
  grpc::Status GetPerceptionRoles(grpc::ServerContext*, const google::protobuf::Empty*,
                                  island::eos::v1::PerceptionRolesResponse*) override;

 private:
  NorthboundCore& core_;
};

}  // namespace eos::runtime::northbound
