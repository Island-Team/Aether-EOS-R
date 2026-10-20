/// Periodically submits camera RGB and optional aligned depth to one pose model.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "eos/hardware/backend.h"
#include "src/eos/runtime/models/runtime.h"

namespace eos::runtime::models {

std::vector<std::uint8_t> EncodeAlignedDepthPng(const std::vector<std::uint16_t>& pixels,
                                                std::uint32_t width, std::uint32_t height);
void AddAlignedKeypointDepths(nlohmann::json& response, const hardware::CameraTelemetry& frame);

class CameraPoseStore final {
 public:
  enum class Status { kNotConfigured, kPending, kComplete, kUnavailable };

  struct ModelState {
    Status status{Status::kNotConfigured};
    std::uint64_t sequence{0};
    std::uint64_t source_generation{0};
    std::uint64_t requested_generation{0};
  };

  struct FaceSchedulingState {
    std::uint64_t human_sequence{0};
    bool human_complete{false};
    bool candidate_present{false};
    bool identity_known{false};
  };

  /// A recently recognized identity safely associated with one current human track.
  struct FaceIdentity {
    std::string identity;
    double similarity{0.0};
    std::uint64_t source_generation{0};
  };

  struct Snapshot {
    std::string model_id;
    std::string device_id;
    std::uint64_t generation{0};
    std::string frame_id;
    std::optional<std::int64_t> source_timestamp_ns;
    hardware::SourceClockKind source_clock{hardware::SourceClockKind::kUnknown};
    std::string source_clock_id;
    bool depth_available{false};
    bool face_requested{false};
    std::uint32_t image_width_px{0};
    std::uint32_t image_height_px{0};
    nlohmann::json response;
    std::uint64_t sequence{0};
    std::shared_ptr<const std::vector<std::uint8_t>> jpeg;
    std::shared_ptr<const std::vector<std::uint8_t>> depth_png;
    std::shared_ptr<const std::vector<std::uint16_t>> depth_u16;
    double depth_scale{0.001};
    std::optional<hardware::CameraIntrinsics> depth_intrinsics;
    std::optional<hardware::GimbalTelemetry> capture_gimbal;
    std::optional<std::int64_t> capture_gimbal_sampled_monotonic_ns;
    std::shared_ptr<const void> opaque_context;
    std::optional<nlohmann::json> face_response;
    std::optional<nlohmann::json> face_pose_response;
    std::optional<std::uint64_t> face_generation;
    std::optional<std::chrono::steady_clock::time_point> face_completed_at;
    std::optional<std::int64_t> face_age_ms;
    std::map<std::string, FaceIdentity> face_identities;
    std::optional<std::int64_t> pose_source_timestamp_ns;
    hardware::SourceClockKind pose_source_clock{hardware::SourceClockKind::kUnknown};
    std::string pose_source_clock_id;
    std::optional<std::chrono::steady_clock::time_point> pose_observed_at;
    std::optional<std::int64_t> pose_age_ms;
    ModelState human;
    ModelState pose;
    ModelState action;
    ModelState face;
  };

  using Updated = std::function<void()>;

  void Set(Snapshot snapshot);
  void SetSource(Snapshot snapshot, bool pose_enabled, bool pose_requested, bool face_enabled,
                 bool face_requested);
  bool SetHuman(std::uint64_t generation, nlohmann::json response);
  bool SetHumanUnavailable(std::uint64_t generation);
  void SetPose(Snapshot snapshot, bool wait_for_face = false);
  void SetDetection(Snapshot snapshot, bool wait_for_face = false);
  bool MergePose(std::uint64_t generation, nlohmann::json response,
                 std::optional<std::int64_t> source_timestamp_ns = std::nullopt,
                 hardware::SourceClockKind source_clock = hardware::SourceClockKind::kUnknown,
                 std::string source_clock_id = {},
                 std::optional<std::chrono::steady_clock::time_point> observed_at = std::nullopt);
  bool SetGemmaPosture(std::uint64_t generation, std::string track_id,
                       std::string posture_state, std::string action,
                       std::uint64_t input_revision = 0);
  std::uint64_t TrackInputRevision(std::string_view track_id) const;
  bool SetPoseUnavailable(std::uint64_t generation);
  bool RepublishLatest();
  std::uint64_t NextGeneration();
  void SetFace(std::uint64_t generation, nlohmann::json response);
  void SetFaceUnavailable(std::uint64_t generation);
  std::optional<Snapshot> Latest() const;
  FaceSchedulingState FaceRequestState() const;
  std::optional<Snapshot> Find(std::string_view device_id, std::uint64_t sequence,
                               std::uint64_t generation) const;
  void RetainExactForFollowup(Snapshot snapshot) const;
  void ReleaseExactFollowup(std::string_view device_id, std::uint64_t sequence,
                            std::uint64_t generation) const;
  void SetUpdatedCallback(Updated updated);

 private:
  mutable std::mutex mutex_;
  mutable std::mutex update_mutex_;
  std::optional<Snapshot> latest_;
  void ApplyFaceObservation(Snapshot& snapshot, const Snapshot& source,
                            const nlohmann::json& response,
                            std::chrono::steady_clock::time_point completed_at);
  void UpdateFaceIdentityCache(const Snapshot& source, const nlohmann::json& response);
  void ResolveFaceIdentities(Snapshot& snapshot);
  void Publish(Snapshot snapshot);
  bool SetUnavailable(std::uint64_t generation, ModelState Snapshot::* state,
                      std::uint64_t* sequence);
  void UpdateRecent();
  void ApplyGemmaPosture(nlohmann::json* response);
  void ObserveTrackInputs(const nlohmann::json& persons);

  std::map<std::uint64_t, Snapshot> pending_;
  std::map<std::uint64_t, Snapshot> source_history_;
  struct FaceResult {
    nlohmann::json response;
    std::chrono::steady_clock::time_point completed_at;
  };
  std::map<std::uint64_t, FaceResult> early_face_results_;
  struct CachedFaceIdentity {
    std::string identity;
    double similarity{0.0};
    std::uint64_t source_generation{0};
  };
  std::map<std::string, CachedFaceIdentity> face_identity_cache_;
  std::deque<Snapshot> recent_;
  struct ExactFollowup {
    Snapshot snapshot;
    std::chrono::steady_clock::time_point expires_at;
  };
  mutable std::deque<ExactFollowup> exact_followups_;
  std::uint64_t sequence_{0};
  std::uint64_t generation_{0};
  std::uint64_t human_sequence_{0};
  std::uint64_t pose_sequence_{0};
  std::uint64_t action_sequence_{0};
  std::uint64_t face_sequence_{0};
  Updated updated_;
  struct GemmaObservation {
    std::string track_id;
    std::string posture_state;
    std::string action;
    std::chrono::steady_clock::time_point completed_at;
  };
  std::map<std::string, GemmaObservation> gemma_observations_;
  std::map<std::string, std::uint64_t> track_first_generation_;
  std::map<std::string, std::string> track_input_signatures_;
  std::map<std::string, std::uint64_t> track_input_revisions_;
};

/// Owns a bounded periodic capture/model loop. `capture` is a generic hardware
/// seam so this layer consumes camera telemetry without knowing any backend.
class CameraPosePump final {
 public:
  using Capture = std::function<std::optional<hardware::CameraTelemetry>()>;

  CameraPosePump(Runtime& models, Capture capture, CameraPoseStore& store, std::string model_id,
                 std::string device_id, std::chrono::milliseconds interval,
                 std::string face_model_id = {}, std::string pose_model_id = {},
                 std::string gemma_model_id = {});
  ~CameraPosePump();
  CameraPosePump(const CameraPosePump&) = delete;
  CameraPosePump& operator=(const CameraPosePump&) = delete;
  void SetPoseEnabled(bool enabled, std::chrono::milliseconds minimum_interval = {});
  void SetFaceEnabled(bool enabled, std::chrono::milliseconds minimum_interval = {});

 private:
  void Run();
  void RunPose();
  void RunPoseEnrichment();
  void RunFace();
  void RunGemma();

  struct PoseJob {
    std::uint64_t generation{0};
    hardware::CameraTelemetry frame;
    std::shared_ptr<const std::string> jpeg_b64;
    std::shared_ptr<const std::string> depth_b64;
    nlohmann::json human_response;
    bool pose_requested{false};
    std::chrono::steady_clock::time_point captured_at;
  };

  struct AuxiliaryJob {
    std::uint64_t generation{0};
    std::shared_ptr<const std::string> jpeg_b64;
    std::chrono::steady_clock::time_point captured_at;
    std::chrono::steady_clock::time_point queued_at;
  };
  struct GemmaJob {
    std::uint64_t generation{0};
    std::string track_id;
    nlohmann::json bbox;
    std::shared_ptr<const std::string> jpeg_b64;
    std::uint64_t input_revision{0};
  };

  Runtime& models_;
  Capture capture_;
  CameraPoseStore& store_;
  std::string model_id_;
  std::string device_id_;
  std::string pose_model_id_;
  std::string face_model_id_;
  std::string gemma_model_id_;
  std::chrono::milliseconds interval_;
  std::mutex mutex_;
  std::condition_variable wakeup_;
  std::condition_variable pose_wakeup_;
  std::condition_variable face_wakeup_;
  std::condition_variable enrichment_wakeup_;
  std::condition_variable gemma_wakeup_;
  std::optional<PoseJob> pending_pose_;
  std::optional<PoseJob> pending_enrichment_;
  std::optional<AuxiliaryJob> pending_face_;
  std::optional<GemmaJob> pending_gemma_;
  std::string stable_largest_track_;
  std::size_t stable_largest_frames_{0};
  nlohmann::json stable_largest_bbox_;
  std::chrono::steady_clock::time_point stable_largest_since_{};
  std::set<std::string> gemma_completed_tracks_;
  std::map<std::string, std::chrono::steady_clock::time_point> gemma_last_request_;
  std::map<std::string, std::string> gemma_track_signatures_;
  std::set<std::string> gemma_refresh_pending_;
  bool gemma_busy_{false};
  bool detection_first_{false};
  bool pose_enabled_{false};
  bool face_enabled_{false};
  std::chrono::milliseconds pose_minimum_interval_{0};
  std::chrono::milliseconds face_minimum_interval_{0};
  std::chrono::steady_clock::time_point next_pose_request_{};
  std::chrono::steady_clock::time_point next_face_request_{};
  std::uint64_t last_face_human_sequence_{0};
  bool face_candidate_present_{false};
  bool face_identity_known_{false};
  std::chrono::steady_clock::time_point next_face_wait_log_{};
  bool stopping_{false};
  std::thread worker_;
  std::thread pose_worker_;
  std::thread enrichment_worker_;
  std::thread face_worker_;
  std::thread gemma_worker_;
  struct Metrics {
    std::uint64_t captured{0}, submitted{0}, replaced{0}, completed{0}, failed{0};
    std::int64_t capture_ms{0}, encoding_ms{0}, inference_ms{0}, total_ms{0};
    std::chrono::steady_clock::time_point window_started{std::chrono::steady_clock::now()};
  } metrics_;
  struct FaceMetrics {
    std::uint64_t queued{0}, replaced{0}, started{0}, completed{0}, failed{0}, stale{0};
    std::int64_t last_queue_wait_ms{0}, last_inference_ms{0}, last_frame_age_ms{0};
    std::chrono::steady_clock::time_point window_started{std::chrono::steady_clock::now()};
  } face_metrics_;
};

}  // namespace eos::runtime::models
