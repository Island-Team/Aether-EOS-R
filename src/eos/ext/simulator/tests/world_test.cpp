#include "src/eos/ext/simulator/world.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>

#include "src/eos/ext/simulator/scene.h"
#include "src/eos/ext/simulator/simulator.h"

namespace {
class FixedCameraSource final : public eos::ext::simulator::CameraFrameSource {
 public:
  explicit FixedCameraSource(std::vector<std::uint8_t> jpeg) : jpeg_(std::move(jpeg)) {}
  std::optional<eos::ext::simulator::CapturedCameraFrame> ReadFrame() const override {
    return eos::ext::simulator::CapturedCameraFrame{
        .frame = {.width_px = 1, .height_px = 1, .jpeg = jpeg_}, .source_timestamp_ns = 42};
  }

 private:
  std::vector<std::uint8_t> jpeg_;
};
}  // namespace

int main() {
  eos::ext::simulator::World world;
  const auto initial = world.CurrentState();
  assert(initial.state_sequence > 0);
  assert(world.StateAt(initial.state_sequence).has_value());
  world.DriveBase(1.0, 0.0, 0.0);
  const auto commanded = world.CurrentState();
  assert(commanded.state_sequence > initial.state_sequence);
  assert(commanded.base.vx == 1.0);
  world.Tick(0.5);
  assert(world.base().x == 0.5);
  const auto moved = world.CurrentState();
  assert(moved.state_sequence > commanded.state_sequence);
  assert(moved.base.x == 0.5);
  world.PointGimbal(0.25, -0.1, 0.0, 0.0);
  eos::ext::simulator::Scene media_scene;
  media_scene.revision = 17;
  media_scene.lines.push_back({.id = "line-a",
                               .source_id = "source-a",
                               .center = {2.0, 0.5},
                               .yaw_rad = 1.5707963267948966,
                               .length_m = 2.0});
  const auto state = std::filesystem::temp_directory_path() / "eos-simulator-world-source-test";
  std::filesystem::remove_all(state);
  const std::vector<std::uint8_t> jpeg{0xff, 0xd8, 0xff, 0xc0, 0x00, 0x07, 0x08,
                                       0x00, 0x01, 0x00, 0x01, 0xff, 0xd9};
  eos::ext::simulator::SourceLibrary library(state);
  library.Write("source-a", "image/jpeg", jpeg, 0);
  eos::ext::simulator::RasterMap monitor_map =
      eos::ext::simulator::RasterMap::EmptyWhite(20, 10, 0.2);
  world.SetSimulator(std::move(monitor_map), std::move(media_scene), state);
  const auto scene = world.CurrentState();
  assert(scene.gimbal.pan_rad == 0.25);
  assert(scene.scene_revision == 17);
  assert(scene.selected_line_id == "line-a");
  assert(scene.selected_source_id == "source-a");
  const auto frame = world.ReadCameraSource();
  assert(frame && frame->jpeg == jpeg && frame->state.state_sequence == scene.state_sequence);
  world.SetCameraSource(std::make_shared<FixedCameraSource>(jpeg));
  const auto system_state = world.CurrentState();
  assert(!system_state.selected_line_id && !system_state.selected_source_id);
  const auto system_frame = world.ReadCameraSource();
  assert(system_frame && system_frame->jpeg == jpeg && system_frame->source_timestamp_ns == 42);
  assert(!world.SimulatorState().camera_hit);
  world.SetCameraSource(nullptr);
  assert(world.CurrentState().selected_source_id == "source-a");
  const auto simulator = world.SimulatorState();
  assert(simulator.map.id == "fallback");
  assert(simulator.map.width_px == 20);
  assert(simulator.scene.revision == 17);
  assert(simulator.camera_hit && simulator.camera_hit->line_id == "line-a");
  const auto raster = world.SimulatorRasterPng();
  assert(raster.map_id == "fallback");
  assert(raster.png.size() > 8);
  assert(raster.png[0] == 0x89 && raster.png[1] == 'P' && raster.png[2] == 'N' &&
         raster.png[3] == 'G');

  std::thread ticking([&world] {
    for (int i = 0; i < 100; ++i) world.Tick(0.01);
  });
  for (int i = 0; i < 100; ++i) {
    const auto concurrent = world.SimulatorState();
    assert(concurrent.map.width_px == 20);
    assert(concurrent.scene.revision == 17);
  }
  ticking.join();

  world.SetBasePose(0.5, 10.0, 0.0);
  const auto no_wall = world.CurrentState();
  assert(no_wall.scene_revision == 17);
  assert(!no_wall.selected_line_id);
  assert(!no_wall.selected_source_id);
  assert(!world.ReadCameraSource());

  for (int i = 0; i < 300; ++i) world.SetBasePosture(static_cast<std::uint8_t>(i % 3));
  assert(!world.StateAt(initial.state_sequence).has_value());
  assert(world.StateAt(world.CurrentState().state_sequence).has_value());
  std::filesystem::remove_all(state);

  world.MoveGripper(true, 5.0);
  assert(world.gripper().state == eos::ext::simulator::GripperJaw::kClosed);
  world.Halt();
  assert(!world.available());
  assert(!world.health().alive);
  assert(!world.health().admitted);
}
