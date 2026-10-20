#include "world.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "scene.h"
#include "simulator.h"
#include "src/eos/lib/log.h"

namespace eos::ext::simulator {
namespace {
constexpr std::size_t kStateHistoryLimit = 256;
constexpr std::size_t kSourceCacheLimit = 64U * 1024U * 1024U;

double Normalize(double value) {
  constexpr double kPi = 3.14159265358979323846;
  while (value > kPi) value -= 2.0 * kPi;
  while (value < -kPi) value += 2.0 * kPi;
  return value;
}
}  // namespace

World::World(std::vector<VisionObject> objects)
    : objects_(std::move(objects)), scene_(std::make_shared<const Scene>()) {
  arm_.ee_orientation_quat[3] = 1.0;
  for (int i = 0; i < 64; ++i) microphone_.samples[i] = static_cast<std::int16_t>(i * 512);
  Detect();
  const RasterMap fallback = RasterMap::EmptyWhite();
  simulator_map_ = fallback.metadata();
  simulator_raster_png_ = fallback.EncodePng();
  raster_map_ = std::make_shared<const RasterMap>(fallback);
  RecordStateLocked();
  eos::lib::LogEvent(eos::lib::GetLogger("demo"), spdlog::level::debug, "simulator_world_started",
                     {{"objects", objects_.size()}});
}

void World::SetCartesian(const double p[3], const double q[4], const double v[3]) {
  std::lock_guard lock(mutex_);
  std::memcpy(arm_.ee_position, p, sizeof(arm_.ee_position));
  std::memcpy(arm_.ee_orientation_quat, q, sizeof(arm_.ee_orientation_quat));
  std::memcpy(arm_.ee_linear_velocity, v, sizeof(arm_.ee_linear_velocity));
}
void World::SetJointPosition(std::size_t i, double value) {
  std::lock_guard lock(mutex_);
  if (i < 6) arm_.joint_positions_rad[i] = value;
}
void World::SetJointVelocity(std::size_t i, double value) {
  std::lock_guard lock(mutex_);
  if (i < 6) arm_.joint_velocities[i] = value;
}
void World::StopArm() {
  std::lock_guard lock(mutex_);
  std::fill(std::begin(arm_.ee_linear_velocity), std::end(arm_.ee_linear_velocity), 0.0);
  std::fill(std::begin(arm_.joint_velocities), std::end(arm_.joint_velocities), 0.0);
}
void World::DriveBase(double vx, double vy, double yaw_rate) {
  std::lock_guard lock(mutex_);
  base_.vx = vx;
  base_.vy = vy;
  base_.yaw_rate = yaw_rate;
  RecordStateLocked();
}
void World::NavigateBaseRelative(double x_m, double y_m, double yaw_rad) {
  std::lock_guard lock(mutex_);
  const double c = std::cos(base_.yaw), s = std::sin(base_.yaw);
  base_.x += c * x_m - s * y_m;
  base_.y += s * x_m + c * y_m;
  base_.yaw = Normalize(base_.yaw + yaw_rad);
  base_.vx = base_.vy = base_.yaw_rate = 0.0;
  RecordStateLocked();
}

bool World::NavigateBaseRelativeChecked(double x_m, double y_m, double yaw_rad,
                                        double footprint_width_m, double footprint_length_m) {
  std::lock_guard lock(mutex_);
  const double c = std::cos(base_.yaw), s = std::sin(base_.yaw);
  const PlanarPose start{base_.x, base_.y, base_.yaw};
  const PlanarPose end{base_.x + c * x_m - s * y_m, base_.y + s * x_m + c * y_m,
                       Normalize(base_.yaw + yaw_rad)};
  if (raster_map_ &&
      raster_map_->CheckSweptFootprint(start, end, footprint_width_m, footprint_length_m))
    return false;
  base_.x = end.x_m;
  base_.y = end.y_m;
  base_.yaw = end.yaw_rad;
  base_.vx = base_.vy = base_.yaw_rate = 0.0;
  RecordStateLocked();
  return true;
}
void World::SetBasePose(double x_m, double y_m, double yaw_rad) {
  std::lock_guard lock(mutex_);
  base_.x = x_m;
  base_.y = y_m;
  base_.yaw = Normalize(yaw_rad);
  base_.vx = base_.vy = base_.yaw_rate = 0.0;
  RecordStateLocked();
}
void World::SetBasePosture(std::uint8_t posture) {
  std::lock_guard lock(mutex_);
  base_.posture = posture;
  RecordStateLocked();
}
void World::StopBase() {
  std::lock_guard lock(mutex_);
  base_.vx = base_.vy = base_.yaw_rate = 0.0;
  RecordStateLocked();
}
void World::PointGimbal(double pan, double tilt, double pr, double tr) {
  std::lock_guard lock(mutex_);
  gimbal_ = {pan, tilt, pr, tr};
  Detect();
  RecordStateLocked();
}
void World::StopGimbal() {
  std::lock_guard lock(mutex_);
  gimbal_.pan_rate = gimbal_.tilt_rate = 0.0;
  RecordStateLocked();
}
void World::UpdateVision(double x, double y, double yaw) {
  std::lock_guard lock(mutex_);
  vision_base_x_ = x;
  vision_base_y_ = y;
  vision_base_yaw_ = yaw;
  Detect();
}
void World::Detect() {
  detections_.clear();
  for (const auto& o : objects_) {
    const double dx = o.x_m - vision_base_x_, dy = o.y_m - vision_base_y_;
    detections_.push_back({o.id, o.label, o.pickable, o.unpickable_reason, std::hypot(dx, dy),
                           Normalize(std::atan2(dy, dx) - vision_base_yaw_ - gimbal_.pan_rad),
                           o.confidence});
  }
}
bool World::RemoveObject(const std::string& id) {
  std::lock_guard lock(mutex_);
  auto it =
      std::find_if(objects_.begin(), objects_.end(), [&](const auto& o) { return o.id == id; });
  if (it == objects_.end()) return false;
  objects_.erase(it);
  Detect();
  return true;
}
void World::AddObject(VisionObject object) {
  std::lock_guard lock(mutex_);
  objects_.push_back(std::move(object));
  Detect();
}
void World::MoveGripper(bool close, double force) {
  std::lock_guard lock(mutex_);
  if (close) {
    gripper_.state = armed_grasp_id_.empty() ? GripperJaw::kClosed : GripperJaw::kHolding;
    gripper_.object_held_id = armed_grasp_id_;
    armed_grasp_id_.clear();
    gripper_.position_pct = 0.0;
  } else {
    gripper_.state = GripperJaw::kOpen;
    gripper_.object_held_id.clear();
    gripper_.position_pct = 100.0;
  }
  gripper_.force_n = force;
}
void World::ArmGrasp(std::string object_id) {
  std::lock_guard lock(mutex_);
  armed_grasp_id_ = std::move(object_id);
}
void World::StopGripper() {
  std::lock_guard lock(mutex_);
  gripper_.force_n = 0.0;
}
void World::PlayAudio(std::size_t bytes) {
  std::lock_guard lock(mutex_);
  speaker_.remaining_s = std::min(10.0, std::max(0.3, static_cast<double>(bytes) / 32000.0));
  speaker_.playing = true;
}
void World::StopSpeaker() {
  std::lock_guard lock(mutex_);
  speaker_.playing = false;
}
void World::Tick(double dt) {
  if (!std::isfinite(dt) || dt < 0.0)
    throw std::invalid_argument("demo tick must be finite and non-negative");
  std::lock_guard lock(mutex_);
  session_elapsed_s_ += dt;
  const bool base_moves =
      dt != 0.0 && (base_.vx != 0.0 || base_.vy != 0.0 || base_.yaw_rate != 0.0);
  const double c = std::cos(base_.yaw), s = std::sin(base_.yaw);
  base_.x += (c * base_.vx - s * base_.vy) * dt;
  base_.y += (s * base_.vx + c * base_.vy) * dt;
  base_.yaw += base_.yaw_rate * dt;
  if (speaker_.playing && (speaker_.remaining_s -= dt) <= 0.0) {
    speaker_.remaining_s = 0.0;
    speaker_.playing = false;
  }
  if (base_moves) RecordStateLocked();
}

void World::RecordStateLocked() {
  const auto line = camera_source_
                        ? std::nullopt
                        : SelectSourceLine(*scene_, {base_.x, base_.y}, base_.yaw + gimbal_.pan_rad,
                                           camera_max_range_m_);
  state_history_.push_back(
      StateSnapshot{.state_sequence = next_state_sequence_++,
                    .base = base_,
                    .gimbal = gimbal_,
                    .scene_revision = scene_->revision,
                    .selected_line_id = line ? std::optional{line->line_id} : std::nullopt,
                    .selected_source_id = line ? std::optional{line->source_id} : std::nullopt});
  if (state_history_.size() > kStateHistoryLimit) state_history_.pop_front();
}

StateSnapshot World::CurrentState() const {
  std::lock_guard lock(mutex_);
  return state_history_.back();
}

std::optional<StateSnapshot> World::StateAt(std::uint64_t sequence) const {
  std::lock_guard lock(mutex_);
  const auto found = std::find_if(
      state_history_.begin(), state_history_.end(),
      [sequence](const StateSnapshot& state) { return state.state_sequence == sequence; });
  if (found == state_history_.end()) return std::nullopt;
  return *found;
}

void World::SetScene(Scene scene, const std::filesystem::path& state_root,
                     double camera_max_range_m) {
  if (!std::isfinite(camera_max_range_m) || camera_max_range_m <= 0.0)
    throw std::invalid_argument("camera max range must be finite and positive");
  std::lock_guard lock(mutex_);
  scene_ = std::make_shared<const Scene>(std::move(scene));
  source_state_root_ = state_root;
  source_cache_.clear();
  source_cache_bytes_ = 0;
  camera_max_range_m_ = camera_max_range_m;
  RecordStateLocked();
}

void World::SetSimulator(RasterMap map, Scene scene, const std::filesystem::path& state_root,
                         double camera_max_range_m) {
  if (!std::isfinite(camera_max_range_m) || camera_max_range_m <= 0.0)
    throw std::invalid_argument("camera max range must be finite and positive");
  std::vector<std::uint8_t> raster_png = map.EncodePng();
  const RasterMapMetadata metadata = map.metadata();
  std::lock_guard lock(mutex_);
  simulator_map_ = metadata;
  simulator_raster_png_ = std::move(raster_png);
  raster_map_ = std::make_shared<const RasterMap>(std::move(map));
  scene_ = std::make_shared<const Scene>(std::move(scene));
  source_state_root_ = state_root;
  source_cache_.clear();
  source_cache_bytes_ = 0;
  camera_max_range_m_ = camera_max_range_m;
  RecordStateLocked();
}

SimulatorSnapshot World::SimulatorState() const {
  std::lock_guard lock(mutex_);
  const auto hit = camera_source_
                       ? std::nullopt
                       : SelectSourceLine(*scene_, {base_.x, base_.y}, base_.yaw + gimbal_.pan_rad,
                                          camera_max_range_m_);
  return SimulatorSnapshot{.state_sequence = state_history_.back().state_sequence,
                           .map = simulator_map_,
                           .scene = *scene_,
                           .base = base_,
                           .gimbal = gimbal_,
                           .camera_max_range_m = camera_max_range_m_,
                           .camera_hit = hit,
                           .arm = arm_,
                           .gripper = gripper_,
                           .speaker = speaker_};
}

SimulatorRaster World::SimulatorRasterPng() const {
  std::lock_guard lock(mutex_);
  return SimulatorRaster{.map_id = simulator_map_.id,
                         .revision = simulator_map_.revision,
                         .png = simulator_raster_png_};
}

std::optional<CameraSourceFrame> World::ReadCameraSource() const {
  std::unique_lock lock(mutex_);
  if (state_history_.empty()) return std::nullopt;
  const StateSnapshot state = state_history_.back();
  if (camera_source_) {
    const auto source = camera_source_;
    lock.unlock();
    const auto captured = source->ReadFrame();
    if (!captured) return std::nullopt;
    return CameraSourceFrame{.width_px = captured->frame.width_px,
                             .height_px = captured->frame.height_px,
                             .source_timestamp_ns = captured->source_timestamp_ns,
                             .jpeg = captured->frame.jpeg,
                             .state = state};
  }
  if (!state.selected_source_id) return std::nullopt;
  const DecodedMedia* source = LoadSelectedSourceLocked(*state.selected_source_id);
  if (source == nullptr || source->frames.empty()) return std::nullopt;
  std::size_t index = 0;
  if (source->video) {
    const auto requested = static_cast<std::size_t>(session_elapsed_s_ * source->frames_per_second);
    index = std::min(requested, source->frames.size() - 1);
  }
  const EncodedFrame& frame = source->frames[index];
  return CameraSourceFrame{
      .width_px = frame.width_px,
      .height_px = frame.height_px,
      .source_timestamp_ns = static_cast<std::int64_t>(session_elapsed_s_ * 1'000'000'000.0),
      .jpeg = frame.jpeg,
      .state = state};
}

void World::SetCameraSource(std::shared_ptr<CameraFrameSource> source) {
  std::lock_guard lock(mutex_);
  camera_source_ = std::move(source);
  RecordStateLocked();
}

const DecodedMedia* World::LoadSelectedSourceLocked(const std::string& source_id) const {
  auto cached = std::find_if(source_cache_.begin(), source_cache_.end(),
                             [&](const DecodedMedia& media) { return media.id == source_id; });
  if (cached != source_cache_.end()) {
    if (std::next(cached) != source_cache_.end()) {
      DecodedMedia promoted = std::move(*cached);
      source_cache_.erase(cached);
      source_cache_.push_back(std::move(promoted));
    }
    return &source_cache_.back();
  }
  if (source_state_root_.empty()) return nullptr;
  try {
    SourceLibrary library(source_state_root_);
    const auto metadata = library.ReadMetadata(source_id);
    DecodedMedia decoded = DecodeMedia(source_id, library.Resolve(source_id),
                                       metadata.at("media_type").get<std::string>());
    std::size_t decoded_bytes = 0;
    for (const auto& frame : decoded.frames) decoded_bytes += frame.jpeg.size();
    if (decoded_bytes > kSourceCacheLimit) return nullptr;
    while (!source_cache_.empty() && source_cache_bytes_ + decoded_bytes > kSourceCacheLimit) {
      for (const auto& frame : source_cache_.front().frames)
        source_cache_bytes_ -= frame.jpeg.size();
      source_cache_.pop_front();
    }
    source_cache_bytes_ += decoded_bytes;
    source_cache_.push_back(std::move(decoded));
    lib::LogEvent(lib::GetLogger("simulator.source"), spdlog::level::debug,
                  "simulator_source_opened",
                  {{"source_id", source_id}, {"decoded_bytes", decoded_bytes}});
    return &source_cache_.back();
  } catch (const std::exception& error) {
    lib::LogEvent(lib::GetLogger("simulator.source"), spdlog::level::warn,
                  "simulator_source_open_failed",
                  {{"source_id", source_id}, {"reason", error.what()}});
    return nullptr;
  }
}

void World::Halt() {
  std::lock_guard lock(mutex_);
  if (halted_) return;
  std::fill(std::begin(arm_.ee_linear_velocity), std::end(arm_.ee_linear_velocity), 0.0);
  std::fill(std::begin(arm_.joint_velocities), std::end(arm_.joint_velocities), 0.0);
  base_.vx = base_.vy = base_.yaw_rate = 0.0;
  gimbal_.pan_rate = gimbal_.tilt_rate = 0.0;
  gripper_.force_n = 0.0;
  speaker_.playing = false;
  halted_ = true;
  eos::lib::LogEvent(eos::lib::GetLogger("demo"), spdlog::level::warn, "simulator_world_halted");
}

ArmState World::arm() const {
  std::lock_guard lock(mutex_);
  return arm_;
}
BaseState World::base() const {
  std::lock_guard lock(mutex_);
  return base_;
}
GimbalState World::gimbal() const {
  std::lock_guard lock(mutex_);
  return gimbal_;
}
std::vector<VisionObject> World::objects() const {
  std::lock_guard lock(mutex_);
  return objects_;
}
std::vector<VisionDetection> World::detections() const {
  std::lock_guard lock(mutex_);
  return detections_;
}
GripperState World::gripper() const {
  std::lock_guard lock(mutex_);
  return gripper_;
}
SpeakerState World::speaker() const {
  std::lock_guard lock(mutex_);
  return speaker_;
}
MicrophoneBlock World::microphone() const {
  std::lock_guard lock(mutex_);
  return microphone_;
}
bool World::available() const {
  std::lock_guard lock(mutex_);
  return !halted_;
}
Health World::health() const {
  std::lock_guard lock(mutex_);
  return {.alive = !halted_, .admitted = false};
}
}  // namespace eos::ext::simulator
