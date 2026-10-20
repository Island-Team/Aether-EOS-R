#include "src/eos/ext/simulator/source_media.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

#include "src/eos/ext/simulator/scene.h"
#include "src/eos/ext/simulator/world.h"

int main() {
  const auto directory = std::filesystem::temp_directory_path() / "eos-source-media-test";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory / "assets");
  const auto video = directory / "assets/clock.webm";

  const pid_t child = ::fork();
  assert(child >= 0);
  if (child == 0) {
    ::execlp("gst-launch-1.0", "gst-launch-1.0", "-q", "videotestsrc", "num-buffers=3", "!",
             "video/x-raw,width=64,height=48,framerate=30/1", "!", "vp8enc", "!", "webmmux", "!",
             "filesink", ("location=" + video.string()).c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  assert(::waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  const auto media = eos::ext::simulator::DecodeMedia("clock", video, "video/webm");
  assert(media.video);
  assert(media.frames.size() == 3);
  for (const auto& frame : media.frames) {
    assert(frame.width_px == 64 && frame.height_px == 48);
    assert(!frame.jpeg.empty());
  }

  eos::ext::simulator::Scene scene;
  scene.revision = 3;
  scene.lines.push_back({.id = "screen",
                         .source_id = "clock",
                         .center = {2.0, 0.0},
                         .yaw_rad = 1.5707963267948966,
                         .length_m = 1.0});
  eos::ext::simulator::World world;
  std::ifstream source(video, std::ios::binary);
  const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(source),
                                        std::istreambuf_iterator<char>()};
  eos::ext::simulator::SourceLibrary library(directory);
  library.Write("clock", "video/webm", bytes, 0);
  world.SetScene(std::move(scene), directory);
  const auto first = world.ReadCameraSource();
  assert(first && first->state.scene_revision == 3);
  world.Tick(1.0 / 30.0);
  const auto second = world.ReadCameraSource();
  assert(second && second->source_timestamp_ns > first->source_timestamp_ns);
  assert(second->jpeg != first->jpeg);
  world.Tick(60.0);
  const auto held = world.ReadCameraSource();
  assert(held && held->jpeg == media.frames.back().jpeg);
  std::filesystem::remove_all(directory);
}
